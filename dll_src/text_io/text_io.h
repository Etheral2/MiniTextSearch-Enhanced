#ifndef TEXT_IO_H
#define TEXT_IO_H

#include "../../public/public.h"

/*
 * 读取 folder_path 目录下所有 .txt 文件，进行清洗（去标点、转小写）、
 * 分词（支持中英文混合分词：英文按空白分词，中文按字符+二元组分词）、
 * 去停用词（从 stopwords.txt 加载中英文停用词），
 * 结果存入 out_docs（内部会分配内存），
 * total_doc_num 返回文档总数。
 * 返回值：成功读取的文档数（>=0），失败返回 -1。
 */
API int load_clean_text(const char* folder_path, DocInfo** out_docs, int* total_doc_num);

/*
 * 从 stopwords.txt 加载停用词表（模块初始化时调用一次）。
 * 若文件不存在则使用内置默认停用词表。
 * 返回值：加载的停用词数量。
 */
API int load_stopwords(const char* stopwords_path);

/*
 * 遍历倒排索引，收集所有以 prefix 为前缀的词项（最多 max_count 个）。
 * 结果写入 suggestions（外部分配的字符串数组，每个长度为 MAX_WORD_LEN）。
 * 返回值：实际写入的建议数量。
 */
API int get_word_suggestions(WordNode* index, const char* prefix,
                              char suggestions[][MAX_WORD_LEN], int max_count);

#endif
