# MiniTextSearch Enhanced — 增强版轻量文本检索引擎

基于原 MiniTextSearch 项目，新增**中文分词**、**二进制 mmap 索引**、**LRU 查询缓存**、**前端搜索补全+高亮**、**增强日志与计时** 五项核心改进。

## 目录结构

```
MiniSearch_Enhanced/
├── public/public.h                  # 全局共用结构体（新增 LRU 缓存、二进制索引常量）
├── stopwords.txt                    # 【新】中英文停用词表（208词）
├── dll_src/
│   ├── text_io/         (1号)       # 文本读取 + 中英文混合分词 + 停用词加载
│   ├── index_build/     (2号)       # 二进制索引构建 + mmap/Win32 快速加载
│   ├── query_handle/    (3号)       # 查询处理 + CJK 查询分词 + LRU 缓存（100条）
│   ├── rank_score/      (4号)       # TF-IDF 打分与排序（同原版）
│   └── result_out/      (组长)      # 结果输出 + Benchmark + 日志系统
├── main_proj/main.c                 # 主调度程序（支持 suggest 模式 + 管道计时）
├── docs_lib/                        # 测试文档（7篇名著 + 4篇示例）
├── index_store/                     # 运行时自动生成二进制 index.dat
├── logs/                            # 【新】运行时自动生成日志目录
├── app.py                           # Flask 网关（新增 /api/suggest + logging + 计时）
├── index.html                       # 前端（新增补全下拉 + <mark>高亮 + 耗时显示）
├── mini_search.exe                  # 编译产物
└── README.md                        # 本文件
```

## 新增功能

### 1. 中文分词支持
- **UTF-8 自动识别**：根据 UTF-8 编码自动区分 CJK 汉字与 ASCII 文本
- **混合分词策略**：英文按空白/标点分词；中文按 **单字 + 二元组 (bigram)** 分词
- **停用词表**：`stopwords.txt` 含 88 个英文停用词 + 74 个中文停用词，支持 `#` 注释和动态加载
- 查询分词同样支持 CJK（`parse_query_sentence` 内部升级）

### 2. 二进制索引 + mmap 快速加载
- **二进制格式**：Header (12B: magic"MTXT" + version + word_count) + 词项记录
- **Windows**：使用 `CreateFileMapping` + `MapViewOfFile` 实现零拷贝文件映射
- **POSIX**：使用标准 `mmap`（Linux/Mac 编译时自动选择）
- 加载速度显著优于逐行文本解析

### 3. 查询结果 LRU 缓存
- **位置**：在 `query_handle` 和 `rank_score` 之间
- **算法**：哈希表 + 双向链表，O(1) 查找/插入/淘汰
- **容量**：固定 100 条，自动淘汰最久未使用的条目
- **统计**：命中/未命中计数，程序结束前输出

### 4. 前端搜索建议与高亮
- **自动补全**：输入框 `input` 事件调用 `/api/suggest?q=prefix`，下拉展示匹配词项
- **键盘导航**：↑↓ 选择、Enter 确认、Escape 关闭
- **关键词高亮**：搜索结果的摘要中使用 `<mark>` 标签高亮匹配关键词
- 新增路由 `/api/suggest`（从 C 引擎的 `suggest <prefix>` 模式获取数据）

### 5. 日志和基准测试增强
- **C 引擎**：每步管道使用 `QueryPerformanceCounter`（Windows）或 `clock_gettime`（POSIX）高精度计时
- **输出格式**：`[TIMESTAMP] 阶段名: xxx.xxx ms`（同时写入 stdout 和 `logs/search.log`）
- **Flask**：Python `logging` 模块 + `RotatingFileHandler` 写入 `logs/flask.log`
- **前端**：显示"查询耗时：xx ms"

## 编译方法

### 依赖
- **GCC** (MinGW-w64 on Windows, 或系统 gcc on Linux/Mac)
- **Windows**：需安装 MinGW-w64，确保 `gcc` 在 PATH 中
- **Python 依赖**：`pip install flask`

### 一键编译（Windows / MinGW）

```bash
cd MiniSearch_Enhanced
gcc -g -Wall main_proj/main.c \
    dll_src/text_io/text_io.c \
    dll_src/index_build/index_build.c \
    dll_src/query_handle/query_handle.c \
    dll_src/rank_score/rank_score.c \
    dll_src/result_out/result_out.c \
    -o mini_search.exe -lm
```

### 命令行测试

```bash
# 交互式检索
echo "search engine" | ./mini_search.exe

# 搜索建议
./mini_search.exe suggest search

# 中文检索
echo "文本检索" | ./mini_search.exe
```

### 启动 Web 服务

```bash
# 终端 1：启动 Flask（端口 3000）
python app.py

# 终端 2：可选，直接命令行测试
curl "http://127.0.0.1:3000/api/search?q=search+engine"
curl "http://127.0.0.1:3000/api/suggest?q=sea"
```

浏览器打开 `http://127.0.0.1:3000` 即可使用完整前端。

## 测试用例

| 测试场景 | 输入 | 预期结果 |
|---------|------|---------|
| 英文单词 | `engine` | 返回 doc1, doc3, doc2 等 |
| 英文短语 | `search engine` | 返回 doc1 (短语匹配加分) |
| 中文检索 | `文本检索` | 按中文分词返回匹配文档 |
| 混合检索 | `search 引擎` | 中英文混合分词，返回相关文档 |
| 建议补全 | `sea` (前端输入) | 下拉显示 search 等词项 |
| 缓存命中 | 相同查询第二次 | 直接返回缓存结果（控制台显示"缓存命中"） |
| 日志记录 | 任何查询 | `logs/search.log` 记录完整时间线 |

## 向后兼容

- 原有 `/api/search?q=` 返回 JSON 格式不变（`docList` + `benchText`），仅新增 `queryTimeMs` 和 `timestamps` 字段
- 所有模块对外 `API` 函数签名不变（模块隔离规范保持）
- 编译产物仍为单个 `mini_search.exe`（无额外运行时依赖）

## 与原项目的关键差异

| 特性 | 原版 | Enhanced |
|-----|------|----------|
| 分词 | 仅英文空白分词 | 中英文混合 UTF-8 分词 |
| 停用词 | 硬编码 22 个英文 | stopwords.txt 动态加载 208 个 |
| 索引格式 | 文本（逐行 fscanf） | 二进制（mmap 映射） |
| 缓存 | 无 | LRU 100 条 |
| 前端 | 基础搜索 | 补全下拉 + <mark>高亮 + 耗时 |
| 日志 | 无 | C 引擎 + Flask 双层日志 |
| 计时 | clock() 毫秒 | QueryPerformanceCounter 微秒级 |
