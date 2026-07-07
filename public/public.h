#ifndef PUBLIC_H
#define PUBLIC_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===================== 全局常量 ===================== */
#define MAX_WORD_LEN     128     /* 单个单词最大长度 */
#define MAX_FILENAME_LEN 256     /* 文件名最大长度 */
#define MAX_CONTENT_LEN  20480   /* 单篇文档最大字符数（增大以支持中文） */
#define MAX_DOC_WORDS    8192    /* 单篇文档最多分词数（增大以支持中文） */
#define MAX_KEYWORDS     256     /* 单次查询最多关键词数 */

/* ===================== 增强功能常量 ===================== */
#define MAX_STOPWORDS    2048    /* 停用词表最大条数 */
#define MAX_STOPWORD_LEN 64      /* 单个停用词最大长度 */
#define LRU_CACHE_SIZE   100     /* LRU 缓存最大条目数 */
#define MAX_QUERY_LEN    1024    /* 查询字符串最大长度 */
#define MAX_SUGGEST_WORDS 20     /* 搜索建议最大返回数 */
#define MAX_LOG_LINE     512     /* 日志单行最大长度 */

/* 二进制索引文件魔法数字与版本号 */
#define INDEX_MAGIC      0x5458544D  /* "MTXT" (little-endian) */
#define INDEX_VERSION    1

/* =====================================================
   跨平台 DLL 导出宏：
   - 默认（单可执行文件编译）：API 展开为空
   - 定义 DLL_EXPORT 时（编译为 .dll）：API 展开为 __declspec(dllexport)
   - 定义 DLL_IMPORT 时（从 .dll 导入）：API 展开为 __declspec(dllimport)
   - Linux/Mac 下始终为空
   ===================================================== */
#ifdef _WIN32
    #ifdef DLL_EXPORT
        #define API __declspec(dllexport)
    #elif defined(DLL_IMPORT)
        #define API __declspec(dllimport)
    #else
        #define API
    #endif
#else
    #define API
#endif

/* 单词位置：文档ID + 单词在文档内的下标（第几个词，从0开始） */
typedef struct WordPos
{
    int docId;
    int pos;
} WordPos;

/* 倒排索引链表节点：一个单词 -> 出现过的所有(文档,位置) */
typedef struct WordNode
{
    char word[MAX_WORD_LEN];
    WordPos* posList;      /* 动态数组 */
    int posCnt;             /* 当前已用个数 */
    int posCap;             /* 动态数组容量（内部使用，读写索引文件时不保存也可重建） */
    struct WordNode* next;
} WordNode;

/* 单篇文档完整信息 */
typedef struct DocInfo
{
    int id;
    char fileName[MAX_FILENAME_LEN];
    char content[MAX_CONTENT_LEN];
    char** words;      /* 分词结果，动态数组，每个元素是一个动态字符串 */
    int wordNum;
} DocInfo;

/* ===================== 增强：LRU缓存结构 ===================== */

/* 检索结果评分项（从 rank_score 移至此处以供缓存使用） */
typedef struct ScoreItem
{
    int docId;
    double score;
} ScoreItem;

/* LRU 缓存条目（双向链表节点 + 哈希表桶） */
typedef struct CacheEntry
{
    char query[MAX_QUERY_LEN];      /* 缓存键：原始查询字符串 */
    ScoreItem* results;             /* 缓存的排序结果数组 */
    int resultCount;                /* 结果数量 */
    struct CacheEntry* prev;        /* LRU 双向链表前驱（最近使用方向） */
    struct CacheEntry* next;        /* LRU 双向链表后继（最久未使用方向） */
    struct CacheEntry* hashNext;    /* 哈希桶内链表 */
} CacheEntry;

/* LRU 缓存管理器 */
typedef struct LRUCache
{
    CacheEntry* hashTable[256];     /* 简单哈希表，key -> 桶 */
    CacheEntry* lruHead;            /* 最近使用端（MRU） */
    CacheEntry* lruTail;            /* 最久未使用端（LRU） */
    int count;                      /* 当前缓存条目数 */
    int maxSize;                    /* 最大容量 = LRU_CACHE_SIZE */
} LRUCache;

#endif
