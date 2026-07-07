#ifndef INDEX_BUILD_H
#define INDEX_BUILD_H

#include "../../public/public.h"

/*
 * 根据清洗、分词后的文档数组 doc_arr（共 doc_total 篇），
 * 构建倒排索引，并以二进制格式写入磁盘文件 save_path。
 * 二进制格式：头部(12B) + 词项记录...（支持 mmap 快速加载）
 * 返回值：0 表示成功，非 0 表示失败。
 */
API int build_save_index(DocInfo* doc_arr, int doc_total, const char* save_path);

/*
 * 从二进制索引文件 index_path 使用 mmap 读取索引，
 * 重建为倒排索引链表，返回链表头指针，失败返回 NULL。
 * （Windows 下使用 CreateFileMapping + MapViewOfFile）
 */
API WordNode* load_index_file(const char* index_path);

#endif
