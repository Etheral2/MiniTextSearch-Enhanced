# MiniTextSearch — 轻量级全文搜索引擎

## 项目简介

本项目实现了一个基于 C 语言的轻量级全文搜索引擎，支持中英文混合检索，具备倒排索引、TF-IDF 排序、LRU 查询缓存等功能。前端采用 Flask + HTML 构建搜索界面，支持搜索建议和关键词高亮。

核心特性：
- **中英文混合分词**：UTF-8 自动检测，英文按空格分词，中文按 Unigram + Bigram 分词
- **二进制倒排索引**：mmap 零拷贝加载，小端字节序存储
- **LRU 查询缓存**：哈希表 + 双向链表，O(1) 查找/淘汰，容量 100 条
- **多格式文档支持**：TXT / HTML / PDF / DOCX 解析，全部内置实现无外部依赖
- **Web 前端**：搜索建议下拉、关键词高亮、查询耗时显示

## 目录结构

```
MiniSearch_project/
├── public/public.h                  # 公共数据结构定义
├── stopwords.txt                    # 中英文停用词表（208词）
├── dll_src/
│   ├── text_io/         (模块1)     # 文档读取、多格式解析、中英文分词
│   ├── index_build/     (模块2)     # 倒排索引构建与二进制存储/mmap加载
│   ├── query_handle/    (模块3)     # 查询分词、LRU缓存
│   ├── rank_score/      (模块4)     # TF-IDF 评分排序
│   └── result_out/      (模块5)     # 结果输出、Benchmark、日志系统
├── main_proj/main.c                 # 主调度程序
├── docs_lib/                        # 测试文档集
├── index_store/                     # 运行时索引文件
├── logs/                            # 运行时日志
├── app.py                           # Flask Web 网关
├── index.html                       # 搜索前端页面
└── CMakeLists.txt                   # CMake 构建配置
```

## 编译与运行

### 环境要求
- GCC（MinGW-w64 / 系统 gcc）
- Python 3.x，安装 Flask：`pip install flask`

### 编译 C 引擎

```bash
gcc -g -Wall main_proj/main.c \
    dll_src/text_io/text_io.c \
    dll_src/index_build/index_build.c \
    dll_src/query_handle/query_handle.c \
    dll_src/rank_score/rank_score.c \
    dll_src/result_out/result_out.c \
    -o mini_search.exe -lm
```

也可用 CMake：

```bash
mkdir build && cd build
cmake .. -G "MinGW Makefiles"
cmake --build .
```

### 命令行使用

```bash
# 交互搜索
echo "search engine" | ./mini_search.exe

# 搜索建议（前缀匹配）
./mini_search.exe suggest search

# 中文检索
echo "文本检索" | ./mini_search.exe
```

### 启动 Web 服务

```bash
python app.py
```

浏览器打开 `http://127.0.0.1:3000` 即可使用。

## 核心技术说明

### 1. 分词策略
- **英文**：以空白和标点为分隔符累积成词，转小写后过滤停用词
- **中文**：通过 UTF-8 编码识别 CJK 字符（U+4E00–U+9FFF 等区间），每个单字作为一个词元（Unigram），相邻 CJK 字符两两组合为二元组（Bigram），例如"搜索引擎" → ["搜","搜索","索","索引","引","引擎","擎"]
- **停用词**：`stopwords.txt` 含 208 个中英文停用词，支持 `#` 注释和动态加载

### 2. 倒排索引
- **结构**：链表实现，每个节点包含词项和动态数组（docId, position）
- **存储**：二进制格式，头部 12 字节（Magic "MTXT" + Version + WordCount），后续每条记录 = 词长(1B) + 词(变长) + 位置数(4B) + 位置数组
- **加载**：Windows 使用 `CreateFileMapping`/`MapViewOfFile`，POSIX 使用 `mmap`，实现零拷贝文件映射

### 3. TF-IDF 评分
- **TF**：词项在文档中出现的原始频率
- **IDF**：`log((N+1)/(df+1)) + 1`（平滑处理，避免分母为 0）
- **短语加分**：若查询词位置连续，找文档中连续出现的匹配给予 +5.0 加分
- 使用插入排序（数据量小时比快排更高效）

### 4. LRU 缓存
- 256 桶哈希表 + 双向链表
- 查找、插入、淘汰均为 O(1)
- djb2 哈希函数，固定容量 100 条

### 5. 多格式文档解析
- **HTML**：标签剥离、`<script>`/`<style>` 跳过、实体解码
- **PDF**：扫描 BT/ET 文本块、FlateDecode 解压、字符串提取
- **DOCX**：ZIP 结构解析、Deflate 解压、XML 文本提取
- 所有解析器均为内置实现，使用内嵌 inflate 算法（~150 行），无第三方依赖

## API 接口

| 路由 | 说明 |
|------|------|
| `GET /` | 搜索前端页面 |
| `GET /api/search?q=关键词` | 执行检索，返回 JSON |
| `GET /api/suggest?q=前缀` | 搜索建议，返回最多 20 条 |

搜索结果 JSON 格式：
```json
{
  "docList": [{"rank": 1, "fileName": "doc1.txt", "score": 2.345, "abstract": "..."}],
  "benchText": "...",
  "queryTimeMs": 1.234,
  "timestamps": [{"stage": "...", "timeMs": 1.0}]
}
```

## 单文件打包（PyInstaller）

```bash
# 一键构建（需安装 pip install pyinstaller）
pyinstaller\build.bat

# 产物在 dist/MiniTextSearch.exe，双击即可运行
```
