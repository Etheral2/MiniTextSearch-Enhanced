# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

MiniTextSearch Enhanced — a lightweight C-based full-text search engine with a Flask web frontend. It indexes `.txt` documents and supports Chinese-English mixed tokenization, TF-IDF ranking, binary mmap indexes, and LRU query caching.

## Build & run

```bash
# Build the C engine (MinGW-w64 / GCC)
gcc -g -Wall main_proj/main.c \
    dll_src/text_io/text_io.c \
    dll_src/index_build/index_build.c \
    dll_src/query_handle/query_handle.c \
    dll_src/rank_score/rank_score.c \
    dll_src/result_out/result_out.c \
    -o mini_search.exe -lm

# CLI: interactive search (reads query from stdin)
echo "search engine" | ./mini_search.exe

# CLI: suggest mode (prefix-based word completion)
./mini_search.exe suggest <prefix>

# Start the web server (port 3000)
python app.py
```

Python dependency: `flask` only. Windows builds use MinGW-w64; on Linux/Mac, the same `gcc` command works (POSIX mmap codepaths are selected via `#ifdef`).

## Architecture

The C engine is a **sequential pipeline** of five isolated modules. Each module lives in `dll_src/<name>/` with a `.h` (public API) and `.c` (implementation). All share the single header `public/public.h` for types and constants.

### Data flow (standard search mode)

```
load_clean_text()         text_io      — read *.txt, tokenize (CJK bigram + English whitespace), strip stopwords
    ↓
build_save_index()        index_build — build inverted index, write binary `index.dat`
    ↓
load_index_file()         index_build — mmap the binary index back into a WordNode linked list
    ↓
parse_query_sentence()    query_handle — tokenize the user's query (same CJK logic)
    ↓
lru_cache_lookup()        query_handle — O(1) hash-table + doubly-linked-list cache (capacity 100)
    ↓ (cache miss)
calc_rank_result()        rank_score   — TF-IDF scoring + phrase-match bonus, insertion sort
    ↓
print_rank_result()       result_out   — console output + benchmark
```

The suggest mode (`./mini_search.exe suggest <prefix>`) bypasses the pipeline: it loads the index, calls `get_word_suggestions()` which scans the WordNode linked list for prefix matches.

### Module responsibilities

| Module | Role |
|---|---|
| `text_io` | Document I/O, UTF-8 CJK detection (U+4E00–U+9FFF, extensions A/B), bigram splitting for Chinese, whitespace splitting for English, stopword loading from `stopwords.txt` |
| `index_build` | Binary index serialization (magic `MTXT` / 0x5458544D, little-endian uint32), mmap loading via `CreateFileMapping` (Windows) or `mmap` (POSIX), inverted-index linked-list construction |
| `query_handle` | Query tokenization (duplicated UTF-8 helpers — intentional module isolation), LRU cache (256-bucket hash table + doubly-linked list, score-only results) |
| `rank_score` | TF-IDF scoring: TF = raw term frequency, IDF = log(N/df), plus adjacency bonus for consecutive keyword positions in docs matching query order |
| `result_out` | Console formatting, `benchmark_test()` for preset queries, high-precision timers (`QueryPerformanceCounter` on Windows, `clock_gettime` on POSIX), timestamped logging to `logs/search.log` |

### Key types (all in `public/public.h`)

- `WordNode` — inverted-index linked list node: a word → dynamic array of `WordPos` (docId + position)
- `DocInfo` — one document: id, fileName, raw content, and `char** words` array
- `ScoreItem` — a (docId, score) pair for ranked results
- `LRUCache` / `CacheEntry` — hash table + doubly-linked list for O(1) cache operations

## Python/Flask layer

`app.py` serves as a thin gateway (port 3000, `127.0.0.1`):

- **`/`** — serves `index.html` directly
- **`/api/search?q=`** — pipes the query to `mini_search.exe` via `echo <q> | mini_search.exe`, parses the stdout with regex to extract docList, benchText, queryTimeMs, and per-stage timestamps
- **`/api/suggest?q=`** — invokes `mini_search.exe suggest <prefix>`, returns up to 20 suggestions

The Flask app writes to `logs/flask.log` via `RotatingFileHandler` (1 MB, 3 backups). If the C engine doesn't report a query time, the Python layer falls back to its own `time.time()` measurement.

## Frontend

`index.html` is a single-page app (no framework) with:
- Autocomplete dropdown calling `/api/suggest` on each keystroke
- `<mark>` highlighting of matched keywords in result abstracts
- Arrow-key navigation in the suggestion dropdown
- Query time display

## Important implementation details

- **Module isolation**: C modules do NOT include each other's headers (except `result_out` which re-includes `rank_score.h` for `ScoreItem`). UTF-8 helper functions are duplicated in `text_io.c` and `query_handle.c` — this is intentional, not an oversight.
- **Memory ownership**: `load_clean_text()` allocates `DocInfo*` array and each doc's `words` array. The caller (`main.c`) is responsible for freeing them. Similarly, `load_index_file()` returns a `WordNode*` linked list the caller must free.
- **Binary index format**: 12-byte header (4B magic `MTXT` + 4B version + 4B word_count), then per-word: 4B word_len + UTF-8 word + 4B posCnt + posCnt × 8B (docId u32 + pos u32).
- **Platform ifdefs**: `_WIN32` guards around `windows.h`, `CreateFileMapping`/`MapViewOfFile`, `QueryPerformanceCounter`, and `SetConsoleOutputCP(65001)`. POSIX paths use `mmap`, `clock_gettime`.
- **Standalone executable**: Despite the `dll_src` naming and `API` export macros, the project compiles as a single `.exe` with all modules linked statically. The `API` macro is a no-op in this configuration.
- **Stopwords**: `stopwords.txt` contains 208 entries (English + Chinese). Lines starting with `#` are comments. The file is read at init time; if missing, a small built-in default is used.
- **Cache scope**: The LRU cache stores only the final ranked `ScoreItem[]` arrays keyed by raw query string. Different top-K values would not match even for the same query.
