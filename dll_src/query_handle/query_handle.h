#ifndef QUERY_HANDLE_H
#define QUERY_HANDLE_H

#include "../../public/public.h"

/*
 * 清洗用户输入语句 input_sent（去标点、转小写、分词、去停用词），
 * 支持中英文混合查询分词，
 * 结果关键词写入 out_keywords（外部分配的 [MAX_KEYWORDS][MAX_WORD_LEN] 数组），
 * pos_arr 记录每个关键词在原查询语句中的顺序位置（0,1,2,...，
 * 位置连续即代表这是一个连续短语，供短语检索加分项使用），
 * word_cnt 返回关键词个数。
 * 返回值：0 表示成功。
 */
API int parse_query_sentence(const char* input_sent, char out_keywords[][MAX_WORD_LEN],
                              int* pos_arr, int* word_cnt);

/* ==================== LRU 缓存接口（增强） ==================== */

/*
 * 初始化 LRU 缓存（模块级单例），max_size 为最大容量（如 100）。
 * 调用一次即可，重复调用会先释放已有缓存再重新初始化。
 */
API void lru_cache_init(int max_size);

/*
 * 在缓存中查找 query 对应的结果。命中时 out_rank 被填充，返回缓存的 resultCount；未命中返回 0。
 * top_k 参数：缓存可能保存了不同 top_k 的结果，此处要求一致。
 */
API int lru_cache_lookup(const char* query, int top_k, ScoreItem* out_rank);

/*
 * 将查询及结果写入缓存（若已存在则更新并移至 MRU 端）。
 */
API void lru_cache_insert(const char* query, ScoreItem* results, int result_count);

/*
 * 释放 LRU 缓存占用的全部内存。
 */
API void lru_cache_free(void);

/*
 * 获取缓存统计信息：返回当前条目数，通过 hits/misses 返回命中和未命中次数。
 */
API int lru_cache_stats(int* hits, int* misses);

#endif
