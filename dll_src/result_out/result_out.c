#include "../../public/public.h"
#include "result_out.h"
#include "../text_io/text_io.h"
#include "../index_build/index_build.h"
#include "../query_handle/query_handle.h"
#include <time.h>
#include <stdarg.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* ==================== 高精度计时 ==================== */

API double get_time_ms(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq = {0};
    static int initialized = 0;
    if (!initialized) {
        QueryPerformanceFrequency(&freq);
        initialized = 1;
    }
    LARGE_INTEGER count;
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
#endif
}

/* ==================== 日志系统 ==================== */

static char g_logTimeBuf[64];

/* 生成时间字符串 "2025-07-07 10:30:45.123" */
static const char* log_timestamp(void)
{
#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(g_logTimeBuf, sizeof(g_logTimeBuf),
             "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
#else
    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    /* 毫秒近似 */
    snprintf(g_logTimeBuf, sizeof(g_logTimeBuf),
             "%04d-%02d-%02d %02d:%02d:%02d.000",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);
#endif
    return g_logTimeBuf;
}

API FILE* log_init(const char* log_path)
{
#ifdef _WIN32
    CreateDirectoryA("logs", NULL);
#else
    /* POSIX: 尝试创建目录 */
    mkdir("logs", 0755);
#endif
    FILE* fp = fopen(log_path, "a");
    if (fp == NULL) {
        fprintf(stderr, "[result_out] 无法创建日志文件: %s\n", log_path);
        return NULL;
    }
    fprintf(fp, "========== MiniTextSearch Enhanced 日志开始 [%s] ==========\n",
            log_timestamp());
    fflush(fp);
    return fp;
}

API void log_write(FILE* log_fp, const char* format, ...)
{
    if (log_fp == NULL) return;

    fprintf(log_fp, "[%s] ", log_timestamp());

    va_list args;
    va_start(args, format);
    vfprintf(log_fp, format, args);
    va_end(args);

    fprintf(log_fp, "\n");
    fflush(log_fp);
}

API void log_close(FILE* log_fp)
{
    if (log_fp == NULL) return;
    fprintf(log_fp, "========== MiniTextSearch Enhanced 日志结束 [%s] ==========\n\n",
            log_timestamp());
    fclose(log_fp);
}

/* ==================== 结果输出 ==================== */

API void print_rank_result(ScoreItem* rank_arr, int rank_cnt, DocInfo* doc_arr, int doc_total)
{
    printf("\n----------------- 检索结果 (Top %d) -----------------\n", rank_cnt);
    if (rank_cnt == 0) {
        printf("未找到匹配的文档。\n");
        printf("------------------------------------------------------\n");
        return;
    }

    for (int i = 0; i < rank_cnt; i++) {
        int docId = rank_arr[i].docId;
        DocInfo* doc = NULL;
        for (int d = 0; d < doc_total; d++) {
            if (doc_arr[d].id == docId) {
                doc = &doc_arr[d];
                break;
            }
        }
        if (doc == NULL) continue;

        char snippet[121];
        strncpy(snippet, doc->content, 120);
        snippet[120] = '\0';

        printf("第%d名  文档:%-20s  得分:%.4f\n", i + 1, doc->fileName, rank_arr[i].score);
        printf("      摘要: %s...\n", snippet);
    }
    printf("------------------------------------------------------\n");
}

/* ==================== Benchmark 测试 ==================== */

API void benchmark_test(void)
{
    printf("\n----------------- Benchmark 测速开始 -----------------\n");

    /* 自行加载文档并读取（或重建）索引，用于批量测试 */
    DocInfo* docs = NULL;
    int docTotal = 0;

    double t0 = get_time_ms();
    load_clean_text("./docs_lib", &docs, &docTotal);
    double t1 = get_time_ms();
    printf("文档加载+清洗耗时: %.3f ms  (共 %d 篇文档)\n",
           t1 - t0, docTotal);

    build_save_index(docs, docTotal, "./index_store/index.dat");
    double t2 = get_time_ms();
    printf("索引构建+写盘耗时: %.3f ms\n", t2 - t1);

    WordNode* index_root = load_index_file("./index_store/index.dat");
    double t3 = get_time_ms();
    printf("索引重新加载耗时: %.3f ms\n", t3 - t2);

    /* 预设一批测试关键词/短语（含中文），分别计时检索 */
    const char* testQueries[] = {
        "text", "search index", "keyword document", "engine",
        "data structure", "pride", "love story"
    };
    int testCount = (int)(sizeof(testQueries) / sizeof(testQueries[0]));
    double totalTime = 0.0;

    for (int i = 0; i < testCount; i++) {
        char keywords[MAX_KEYWORDS][MAX_WORD_LEN];
        int posArr[MAX_KEYWORDS];
        int wordCnt = 0;

        char inputCopy[256];
        strncpy(inputCopy, testQueries[i], sizeof(inputCopy) - 1);
        inputCopy[sizeof(inputCopy) - 1] = '\0';

        parse_query_sentence(inputCopy, keywords, posArr, &wordCnt);

        ScoreItem top10[10];
        double qStart = get_time_ms();
        int resultCnt = calc_rank_result(keywords, wordCnt, posArr, index_root, 10, top10);
        double qEnd = get_time_ms();

        double ms = qEnd - qStart;
        totalTime += ms;
        printf("测试查询 \"%s\" 耗时: %.4f ms  (返回 %d 条结果)\n",
               testQueries[i], ms, resultCnt);
    }

    printf("批量测试关键词平均响应时间: %.4f ms  (共 %d 组查询)\n",
           totalTime / testCount, testCount);
    printf("------------------------------------------------------\n");

    /* 释放本次 benchmark 独立加载的资源 */
    for (int d = 0; d < docTotal; d++) {
        for (int w = 0; w < docs[d].wordNum; w++)
            free(docs[d].words[w]);
        free(docs[d].words);
    }
    free(docs);

    WordNode* p = index_root;
    while (p != NULL) {
        WordNode* next = p->next;
        free(p->posList);
        free(p);
        p = next;
    }
}
