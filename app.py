from flask import Flask, request, jsonify
import subprocess
import os
import re
import sys
import shutil
import webbrowser
import threading
import tempfile
import logging
import time
from logging.handlers import RotatingFileHandler

app = Flask(__name__)


# ==================== 路径 & 数据初始化 ====================

def get_app_dir():
    """
    返回应用数据目录（可写）。
    - PyInstaller 打包后：%APPDATA%/MiniTextSearch
    - 开发模式：当前目录
    """
    if getattr(sys, 'frozen', False):
        base = os.path.join(os.environ.get('APPDATA', os.path.expanduser('~')),
                            'MiniTextSearch')
    else:
        base = os.getcwd()
    return base


def init_app_data():
    """
    首次运行时，将 PyInstaller 打包的只读资源（_MEIPASS）复制到可写的 app_dir。
    通过 .init_done 标记文件避免重复复制。
    """
    if not getattr(sys, 'frozen', False):
        return   # 开发模式，无需复制

    meipass = sys._MEIPASS
    app_dir = get_app_dir()

    marker = os.path.join(app_dir, '.init_done')
    if os.path.exists(marker):
        return   # 已初始化

    os.makedirs(app_dir, exist_ok=True)

    # 需要复制到 app_dir 的单个文件
    files_to_copy = ['mini_search.exe', 'stopwords.txt', 'index.html']
    for fname in files_to_copy:
        src = os.path.join(meipass, fname)
        dst = os.path.join(app_dir, fname)
        if os.path.exists(src):
            shutil.copy2(src, dst)

    # 复制文档库（目录）
    src_docs = os.path.join(meipass, 'docs_lib')
    dst_docs = os.path.join(app_dir, 'docs_lib')
    if os.path.exists(src_docs):
        if os.path.exists(dst_docs):
            shutil.rmtree(dst_docs)
        shutil.copytree(src_docs, dst_docs)

    # 创建运行时目录
    os.makedirs(os.path.join(app_dir, 'logs'), exist_ok=True)
    os.makedirs(os.path.join(app_dir, 'index_store'), exist_ok=True)

    # 写入标记文件
    with open(marker, 'w') as f:
        f.write('1')

    print(f"[初始化] 数据文件已复制到 {app_dir}")


# 在模块加载时执行初始化 & 获取 app_dir
init_app_data()
APP_DIR = get_app_dir()
EXE = os.path.join(APP_DIR, "mini_search.exe")
TIMEOUT = 10


# ==================== 日志配置 ====================

os.makedirs(os.path.join(APP_DIR, "logs"), exist_ok=True)
logging.basicConfig(level=logging.INFO)
file_handler = RotatingFileHandler(
    os.path.join(APP_DIR, "logs", "flask.log"),
    maxBytes=1024 * 1024, backupCount=3, encoding="utf-8"
)
file_handler.setFormatter(logging.Formatter(
    "[%(asctime)s] %(levelname)s - %(message)s", datefmt="%Y-%m-%d %H:%M:%S"
))
app.logger.addHandler(file_handler)
app.logger.setLevel(logging.INFO)


# ==================== C 引擎调用 ====================

def run_exe_full_output(keyword):
    """通过管道将关键词传给 C 引擎，返回 stdout 全文"""
    tmp_fd, tmp_path = tempfile.mkstemp(suffix=".txt")
    os.close(tmp_fd)
    cmd = f'(echo {keyword}) | "{EXE}" > "{tmp_path}"'
    subprocess.run(
        cmd,
        shell=True,
        cwd=APP_DIR,
        encoding="utf-8",
        errors="replace",
        timeout=TIMEOUT
    )
    with open(tmp_path, "r", encoding="utf-8", errors="replace") as f:
        full_text = f.read()
    os.unlink(tmp_path)
    return full_text


def run_exe_suggest(prefix):
    """调用 C 引擎的 suggest 模式获取补全词"""
    try:
        result = subprocess.run(
            [EXE, "suggest", prefix],
            cwd=APP_DIR,
            capture_output=True,
            encoding="utf-8",
            errors="replace",
            timeout=5
        )
        lines = result.stdout.strip().split("\n")
        return [line.strip() for line in lines if line.strip()]
    except Exception as e:
        app.logger.warning(f"SUGGEST 调用失败: {e}")
        return []


# ==================== 输出解析 ====================

def parse_text(text):
    """解析 C 引擎输出，提取文档列表、测速数据和耗时信息"""
    if not text:
        return {"docList": [], "benchText": "无程序输出", "queryTimeMs": 0}

    # 提取 TIMESTAMP 行用于耗时展示
    timestamps = []
    for m in re.finditer(r"\[TIMESTAMP\]\s*(.+?):\s*([0-9.]+)\s*ms", text):
        timestamps.append({"stage": m.group(1), "timeMs": round(float(m.group(2)), 3)})

    # 计算查询总耗时
    query_time = 0
    tfidf_match = re.search(r"\[TIMESTAMP\]\s*TF-IDF 完成:\s*([0-9.]+)\s*ms", text)
    tfidf_start = re.search(r"\[TIMESTAMP\]\s*.*?TF-IDF 开始.*?:\s*([0-9.]+)\s*ms", text)
    if tfidf_match and tfidf_start:
        query_time = round(float(tfidf_match.group(1)) - float(tfidf_start.group(1)), 3)

    # 分割基准测试部分
    bench_flag = "----------------- Benchmark 测速开始 -----------------"
    if bench_flag in text:
        res_part, bench_part = text.split(bench_flag, 1)
        bench_data = bench_flag + bench_part
    else:
        res_part = text
        bench_data = ""

    doc_list = []
    pattern = re.compile(
        r"第(\d+)名\s+文档:(.+?)\s+得分:([0-9.]+)\n\s+摘要:\s*(.*?)(?=\n第|\n----|\n\[)",
        re.S
    )
    all_items = pattern.findall(res_part)
    for rk, fn, sc, ab in all_items:
        doc_list.append({
            "rank": int(rk),
            "fileName": fn,
            "score": float(sc),
            "abstract": ab.strip()
        })
    doc_list.sort(key=lambda x: x["score"], reverse=True)
    return {
        "docList": doc_list,
        "benchText": bench_data,
        "queryTimeMs": query_time,
        "timestamps": timestamps
    }


# ==================== API 路由 ====================

@app.route("/api/search")
def search():
    """主搜索接口（向后兼容，返回格式不变）"""
    q = request.args.get("q", "").strip()
    if not q:
        return jsonify({"docList": [], "benchText": "", "queryTimeMs": 0})

    t_start = time.time()
    full_out = run_exe_full_output(q)
    res = parse_text(full_out)
    t_end = time.time()

    # 如果 C 引擎未返回耗时，使用 Python 端计时
    if res["queryTimeMs"] == 0:
        res["queryTimeMs"] = round((t_end - t_start) * 1000, 3)

    app.logger.info(
        f"查询: '{q}' | 结果数: {len(res['docList'])} | "
        f"耗时: {res['queryTimeMs']} ms | 客户端: {request.remote_addr}"
    )

    return jsonify(res)


@app.route("/api/suggest")
def suggest():
    """搜索建议接口（新增）"""
    prefix = request.args.get("q", "").strip()
    if not prefix or len(prefix) < 1:
        return jsonify({"suggestions": []})

    suggestions = run_exe_suggest(prefix)
    return jsonify({"suggestions": suggestions[:20]})


@app.route("/")
def index():
    with open(os.path.join(APP_DIR, "index.html"), "r", encoding="utf-8") as f:
        return f.read()


# ==================== 启动入口 ====================

if __name__ == "__main__":
    print("=" * 56)
    print("  MiniTextSearch Enhanced  文本检索系统")
    print("=" * 56)
    print(f"  数据目录: {APP_DIR}")
    print(f"  服务地址: http://127.0.0.1:3000")
    print(f"  按 Ctrl+C 退出")
    print("=" * 56)

    # 延迟 1.5 秒后自动打开浏览器（给服务器启动留时间）
    def _open_browser():
        webbrowser.open("http://127.0.0.1:3000")

    threading.Timer(1.5, _open_browser).start()

    app.logger.info(f"MiniTextSearch Enhanced 服务启动 (端口 3000) | 数据目录: {APP_DIR}")
    app.run(host="127.0.0.1", port=3000, debug=False)
