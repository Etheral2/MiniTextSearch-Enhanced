#ifndef RANK_SCORE_H
#define RANK_SCORE_H

#include "../../public/public.h"

/* ScoreItem 已定义在 public.h 中，此处不再重复定义 */

/*
 * 根据 keywords（共 word_cnt 个关键词，pos_arr 记录其在原查询中的顺序位置）
 * 在 index_root 倒排索引中检索，计算每篇文档的 TF-IDF 匹配得分
 * （若关键词在查询中连续且在文档中也连续出现，额外加短语匹配加分），
 * 按得分从高到低排序，取前 top_k 篇写入 out_rank（外部分配，大小 >= top_k）。
 * 返回值：实际写入 out_rank 的文档篇数。
 */
API int calc_rank_result(char keywords[][MAX_WORD_LEN], int word_cnt, int* pos_arr,
                          WordNode* index_root, int top_k, ScoreItem* out_rank);

#endif
