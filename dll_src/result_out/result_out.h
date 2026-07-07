#ifndef RESULT_OUT_H
#define RESULT_OUT_H

#include "../../public/public.h"
#include "../rank_score/rank_score.h"

/*
 * 在控制台格式化打印检索结果：文档名称、得分、内容摘要。
 * rank_arr / rank_cnt 是 calc_rank_result 返回的排序结果。
 */
API void print_rank_result(ScoreItem* rank_arr, int rank_cnt, DocInfo* doc_arr, int doc_total);

/*
 * 批量测试：对若干预设关键词分别计时检索，输出平均响应时间（加分项）。
 * 内部会自行从 docs_lib / index_store 重新加载数据进行测试。
 */
API void benchmark_test(void);

/* ==================== 增强：日志与计时 ==================== */

/*
 * 初始化日志文件（logs/search.log），返回日志文件指针（可为 NULL）。
 * 若日志目录不存在则自动创建。
 */
API FILE* log_init(const char* log_path);

/*
 * 写入一条带时间戳的日志。
 */
API void log_write(FILE* log_fp, const char* format, ...);

/*
 * 关闭日志文件。
 */
API void log_close(FILE* log_fp);

/*
 * 返回当前时间戳（毫秒精度），用于管道计时。
 * Windows 使用 QueryPerformanceCounter，POSIX 使用 clock_gettime。
 */
API double get_time_ms(void);

#endif
