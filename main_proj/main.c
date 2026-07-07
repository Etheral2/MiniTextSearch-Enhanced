#include "../public/public.h"
#include "../dll_src/text_io/text_io.h"
#include "../dll_src/index_build/index_build.h"
#include "../dll_src/query_handle/query_handle.h"
#include "../dll_src/rank_score/rank_score.h"
#include "../dll_src/result_out/result_out.h"
#ifdef _WIN32
#include <windows.h>
#endif
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

/* ==================== 时间戳辅助宏 ==================== */
/*
 * TIME_STAMP — 使用 QueryPerformanceCounter 记录程序各阶段耗时。
 * g_baseTime 在程序启动时初始化，TIME_STAMP 输出的是相对于启动时刻的偏移量。
 * 同时写入 stdout 和日志文件，方便调试和性能分析。
 */
#define TIME_STAMP(label, fp) do { \
    double _t = get_time_ms(); \
    printf("[TIMESTAMP] %s: %.3f ms\n", (label), _t - g_baseTime); \
    if (fp) log_write((fp), "%s: %.3f ms", (label), _t - g_baseTime); \
} while(0)

static double g_baseTime = 0.0;

/*
 * 检查索引文件是否过期：遍历 docs_lib，若有文档比索引文件更新，需重建。
 * 返回 1 表示需要重建，0 表示索引是最新的。
 */
static int index_needs_rebuild(const char* index_path, const char* docs_dir)
{
    struct stat idx_st;
    if (stat(index_path, &idx_st) != 0)
        return 1;   /* 索引文件不存在，必须重建 */

    DIR* dir = opendir(docs_dir);
    if (!dir) return 1;

    struct dirent* entry;
    int doc_count = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;  /* 跳过 . .. 和隐藏文件 */

        char full[MAX_FILENAME_LEN + 512];
        snprintf(full, sizeof(full), "%s/%s", docs_dir, entry->d_name);

        struct stat doc_st;
        if (stat(full, &doc_st) == 0) {
            doc_count++;
            /* 比较修改时间：difftime > 0 表示 doc 比 index 新 */
            if (difftime(doc_st.st_mtime, idx_st.st_mtime) > 0)
                { closedir(dir); return 1; }
        }
    }
    closedir(dir);
    /* 如果目录为空也需重建（让后面流程报"未找到文档"） */
    return (doc_count == 0) ? 1 : 0;
}

int main(int argc, char* argv[])
{
#ifdef _WIN32
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#endif

    /* 初始化全局基准时间 */
    g_baseTime = get_time_ms();

    /* 初始化日志系统 */
    FILE* logFp = log_init("logs/search.log");

    /* 初始化 LRU 缓存 */
    lru_cache_init(LRU_CACHE_SIZE);
    log_write(logFp, "系统启动，LRU 缓存已初始化 (capacity=%d)", LRU_CACHE_SIZE);

    TIME_STAMP("系统启动完成", logFp);

    /* ====== 特殊模式：suggest（命令行参数） ====== */
    if (argc >= 3 && strcmp(argv[1], "suggest") == 0) {
        /* 加载索引，输出建议词 */
        DocInfo* tmpDocs = NULL;
        int tmpCount = 0;
        load_clean_text("./docs_lib", &tmpDocs, &tmpCount);
        build_save_index(tmpDocs, tmpCount, "./index_store/index.dat");
        WordNode* suggestIndex = load_index_file("./index_store/index.dat");

        if (suggestIndex) {
            char suggestions[MAX_SUGGEST_WORDS][MAX_WORD_LEN];
            int cnt = get_word_suggestions(suggestIndex, argv[2],
                                           suggestions, MAX_SUGGEST_WORDS);
            for (int i = 0; i < cnt; i++) {
                printf("%s\n", suggestions[i]);
            }
            /* 释放索引 */
            WordNode* sp = suggestIndex;
            while (sp) { WordNode* sn = sp->next; free(sp->posList); free(sp); sp = sn; }
        }

        /* 清理 */
        for (int d = 0; d < tmpCount; d++) {
            for (int w = 0; w < tmpDocs[d].wordNum; w++) free(tmpDocs[d].words[w]);
            free(tmpDocs[d].words);
        }
        free(tmpDocs);
        log_write(logFp, "SUGGEST 模式完成: prefix='%s'", argv[2]);
        log_close(logFp);
        return 0;
    }

    /* ====== 标准检索模式 ====== */

    DocInfo* docList = NULL;
    int docCount = 0;

    /* 步骤1：读取并清洗 docs_lib 下全部文档 */
    TIME_STAMP("开始加载文档", logFp);
    load_clean_text("./docs_lib", &docList, &docCount);
    TIME_STAMP("文档加载完成", logFp);

    if (docCount == 0) {
        printf("警告：docs_lib 目录下未找到任何 .txt 文档，请检查路径或添加测试文档。\n");
        log_write(logFp, "警告：未找到任何文档");
    } else {
        log_write(logFp, "已加载 %d 篇文档", docCount);
    }

    /* 步骤2：构建二进制索引（增量检查，若索引已是最新则跳过重建） */
    TIME_STAMP("开始构建索引", logFp);
    if (index_needs_rebuild("./index_store/index.dat", "./docs_lib")) {
        build_save_index(docList, docCount, "./index_store/index.dat");
        log_write(logFp, "索引已重建");
    } else {
        printf("[main] 索引文件已是最新，跳过重建\n");
        log_write(logFp, "索引文件已是最新，跳过重建");
    }
    TIME_STAMP("索引构建完成", logFp);

    WordNode* indexRoot = load_index_file("./index_store/index.dat");
    TIME_STAMP("索引加载完成 (mmap)", logFp);

    /* 步骤3：接收用户输入搜索语句 */
    char inputBuffer[MAX_QUERY_LEN];
    printf("===== MiniTextSearch Enhanced 文本检索系统 =====\n");
    printf("已索引文档数: %d\n", docCount);
    printf("请输入检索句子/关键词：");
    fflush(stdout);
    if (fgets(inputBuffer, sizeof(inputBuffer), stdin) == NULL) {
        inputBuffer[0] = '\0';
    }

    /* 去除末尾换行 */
    size_t inLen = strlen(inputBuffer);
    while (inLen > 0 && (inputBuffer[inLen-1] == '\n' || inputBuffer[inLen-1] == '\r'))
        inputBuffer[--inLen] = '\0';

    log_write(logFp, "收到查询: '%s'", inputBuffer);

    char queryWordList[MAX_KEYWORDS][MAX_WORD_LEN] = {0};
    int wordPositionArr[MAX_KEYWORDS] = {0};
    int queryWordTotal = 0;

    TIME_STAMP("开始查询解析", logFp);
    parse_query_sentence(inputBuffer, queryWordList, wordPositionArr, &queryWordTotal);
    TIME_STAMP("查询解析完成", logFp);

    log_write(logFp, "解析出 %d 个关键词", queryWordTotal);

    /* 步骤4：TF-IDF 打分（带 LRU 缓存） */
    ScoreItem top10Result[10];
    int rankCnt = 0;

    TIME_STAMP("开始缓存查询", logFp);
    /* 先查缓存 */
    rankCnt = lru_cache_lookup(inputBuffer, 10, top10Result);
    if (rankCnt > 0) {
        log_write(logFp, "缓存命中！返回 %d 条结果", rankCnt);
    } else {
        log_write(logFp, "缓存未命中，执行 TF-IDF 计算");
        /* 缓存未命中，执行完整的 TF-IDF 计算 */
        TIME_STAMP("TF-IDF 开始", logFp);
        rankCnt = calc_rank_result(queryWordList, queryWordTotal, wordPositionArr,
                                    indexRoot, 10, top10Result);
        TIME_STAMP("TF-IDF 完成", logFp);
        /* 写入缓存 */
        lru_cache_insert(inputBuffer, top10Result, rankCnt);
        log_write(logFp, "结果已写入缓存 (共 %d 条)", rankCnt);
    }

    /* 步骤5：控制台打印检索结果 */
    print_rank_result(top10Result, rankCnt, docList, docCount);

    /* 步骤6：性能测速（加分项） */
    TIME_STAMP("开始 Benchmark", logFp);
    benchmark_test();
    TIME_STAMP("Benchmark 完成", logFp);

    /* 缓存统计 */
    int cacheHits = 0, cacheMisses = 0;
    int cacheCount = lru_cache_stats(&cacheHits, &cacheMisses);
    printf("\n[缓存统计] 当前条目: %d | 命中: %d | 未命中: %d\n",
           cacheCount, cacheHits, cacheMisses);
    log_write(logFp, "缓存统计 - 条目:%d 命中:%d 未命中:%d",
              cacheCount, cacheHits, cacheMisses);

    /* 释放主流程中占用的内存 */
    for (int d = 0; d < docCount; d++) {
        for (int w = 0; w < docList[d].wordNum; w++)
            free(docList[d].words[w]);
        free(docList[d].words);
    }
    free(docList);

    WordNode* p = indexRoot;
    while (p != NULL) {
        WordNode* next = p->next;
        free(p->posList);
        free(p);
        p = next;
    }

    /* 释放 LRU 缓存 */
    lru_cache_free();

    TIME_STAMP("系统关闭", logFp);
    log_close(logFp);

#ifdef _WIN32
    system("pause");
#endif
    return 0;
}
