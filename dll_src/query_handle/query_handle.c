#include "../../public/public.h"
#include "query_handle.h"
#include <ctype.h>

/* ==================== UTF-8 辅助 (用于查询分词) ==================== */

static int utf8_char_len(const unsigned char* s)
{
    if (*s < 0x80)            return 1;
    if ((*s & 0xE0) == 0xC0)  return 2;
    if ((*s & 0xF0) == 0xE0)  return 3;
    if ((*s & 0xF8) == 0xF0)  return 4;
    return 0;
}

static int is_cjk_char(const unsigned char* s, int len)
{
    if (len == 3) {
        unsigned int cp = ((unsigned int)(s[0] & 0x0F) << 12)
                        | ((unsigned int)(s[1] & 0x3F) << 6)
                        |  (unsigned int)(s[2] & 0x3F);
        return (cp >= 0x4E00 && cp <= 0x9FFF)
            || (cp >= 0x3400 && cp <= 0x4DBF)
            || (cp >= 0xF900 && cp <= 0xFAFF);
    }
    return 0;
}

static int is_cjk_punct(const unsigned char* s, int len)
{
    if (len == 3) {
        unsigned int cp = ((unsigned int)(s[0] & 0x0F) << 12)
                        | ((unsigned int)(s[1] & 0x3F) << 6)
                        |  (unsigned int)(s[2] & 0x3F);
        return (cp >= 0x3000 && cp <= 0x303F)
            || (cp >= 0xFF00 && cp <= 0xFFEF)
            || cp == 0x3001 || cp == 0x3002;
    }
    return 0;
}

static int is_space_utf8(const unsigned char* s, int len)
{
    if (len == 1 && isspace((int)s[0])) return 1;
    if (len == 3 && s[0] == 0xE3 && s[1] == 0x80 && s[2] == 0x80) return 1;
    return 0;
}

/* ==================== 内部静态工具函数 ==================== */

static void trim_str(char* s)
{
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[len - 1] = '\0';
        len--;
    }
    size_t start = 0;
    while (s[start] != '\0' && isspace((unsigned char)s[start]))
        start++;
    if (start > 0)
        memmove(s, s + start, len - start + 1);
}

static int is_punct_char(char ch)
{
    if (isalnum((unsigned char)ch) || isspace((unsigned char)ch))
        return 0;
    return 1;
}

static void clean_line_ascii(char* line)
{
    for (size_t i = 0; line[i] != '\0'; i++) {
        if (is_punct_char(line[i]))
            line[i] = ' ';
        else
            line[i] = (char)tolower((unsigned char)line[i]);
    }
}

/* ==================== 对外接口实现 ==================== */

API int parse_query_sentence(const char* input_sent, char out_keywords[][MAX_WORD_LEN],
                              int* pos_arr, int* word_cnt)
{
    char buf[MAX_QUERY_LEN];
    strncpy(buf, input_sent, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    trim_str(buf);

    /* ASCII 清洗（转小写、标点→空格），不破坏 UTF-8 多字节序列 */
    clean_line_ascii(buf);

    int count = 0;
    int pos = 0;
    int bufLen = (int)strlen(buf);

    /* CJK 字符暂存 */
    char cjkChars[MAX_KEYWORDS][4];
    int  cjkCount = 0;

    while (pos < bufLen && count < MAX_KEYWORDS) {
        int clen = utf8_char_len((const unsigned char*)(buf + pos));
        if (clen <= 0) { pos++; continue; }

        if (clen == 1) {
            unsigned char ch = (unsigned char)buf[pos];
            if (isspace(ch)) {
                /* 空白：刷新 CJK 缓冲 */
                for (int i = 0; i < cjkCount && count < MAX_KEYWORDS; i++) {
                    strncpy(out_keywords[count], cjkChars[i], MAX_WORD_LEN - 1);
                    out_keywords[count][MAX_WORD_LEN - 1] = '\0';
                    pos_arr[count] = count;
                    count++;
                }
                cjkCount = 0;
                pos++;
            } else {
                /* ASCII 字母/数字 → 累积成词 */
                for (int i = 0; i < cjkCount && count < MAX_KEYWORDS; i++) {
                    strncpy(out_keywords[count], cjkChars[i], MAX_WORD_LEN - 1);
                    out_keywords[count][MAX_WORD_LEN - 1] = '\0';
                    pos_arr[count] = count;
                    count++;
                }
                cjkCount = 0;

                /* 向前扫描连续的 ASCII 字母数字组成一个词 */
                int wordStart = pos;
                while (pos < bufLen) {
                    int nextLen = utf8_char_len((const unsigned char*)(buf + pos));
                    if (nextLen == 1 && !isspace((unsigned char)buf[pos]) && !is_punct_char(buf[pos])) {
                        pos++;
                    } else {
                        break;
                    }
                }
                int wlen = pos - wordStart;
                if (wlen > 0 && count < MAX_KEYWORDS) {
                    int cp = (wlen < MAX_WORD_LEN - 1) ? wlen : (MAX_WORD_LEN - 1);
                    memcpy(out_keywords[count], buf + wordStart, (size_t)cp);
                    out_keywords[count][cp] = '\0';
                    pos_arr[count] = count;
                    count++;
                }
            }
        } else {
            /* 多字节 UTF-8 */
            if (is_space_utf8((const unsigned char*)(buf + pos), clen)) {
                for (int i = 0; i < cjkCount && count < MAX_KEYWORDS; i++) {
                    strncpy(out_keywords[count], cjkChars[i], MAX_WORD_LEN - 1);
                    out_keywords[count][MAX_WORD_LEN - 1] = '\0';
                    pos_arr[count] = count;
                    count++;
                }
                cjkCount = 0;
            } else if (is_cjk_punct((const unsigned char*)(buf + pos), clen)) {
                for (int i = 0; i < cjkCount && count < MAX_KEYWORDS; i++) {
                    strncpy(out_keywords[count], cjkChars[i], MAX_WORD_LEN - 1);
                    out_keywords[count][MAX_WORD_LEN - 1] = '\0';
                    pos_arr[count] = count;
                    count++;
                }
                cjkCount = 0;
            } else if (is_cjk_char((const unsigned char*)(buf + pos), clen)) {
                /* CJK 字符：独立为一个查询词 */
                if (cjkCount < MAX_KEYWORDS && clen <= 4) {
                    memcpy(cjkChars[cjkCount], buf + pos, (size_t)clen);
                    cjkChars[cjkCount][clen] = '\0';
                    cjkCount++;
                }
            }
            pos += clen;
        }
    }
    /* 末尾残留 CJK */
    for (int i = 0; i < cjkCount && count < MAX_KEYWORDS; i++) {
        strncpy(out_keywords[count], cjkChars[i], MAX_WORD_LEN - 1);
        out_keywords[count][MAX_WORD_LEN - 1] = '\0';
        pos_arr[count] = count;
        count++;
    }

    *word_cnt = count;
    return 0;
}

/* ==================== LRU 缓存实现 ==================== */

static LRUCache g_cache;
static int    g_cacheHits = 0;
static int    g_cacheMisses = 0;

/* djb2 哈希 */
static unsigned int hash_str(const char* s)
{
    unsigned int h = 5381;
    int c;
    while ((c = (unsigned char)*s++))
        h = ((h << 5) + h) + (unsigned int)c;
    return h % 256;
}

static CacheEntry* cache_entry_new(const char* query, ScoreItem* results, int count)
{
    CacheEntry* e = (CacheEntry*)malloc(sizeof(CacheEntry));
    strncpy(e->query, query, MAX_QUERY_LEN - 1);
    e->query[MAX_QUERY_LEN - 1] = '\0';
    e->resultCount = count;
    e->results = (ScoreItem*)malloc(sizeof(ScoreItem) * (count > 0 ? count : 1));
    if (count > 0)
        memcpy(e->results, results, sizeof(ScoreItem) * (size_t)count);
    e->prev = NULL;
    e->next = NULL;
    e->hashNext = NULL;
    return e;
}

static void cache_entry_free(CacheEntry* e)
{
    if (e) {
        free(e->results);
        free(e);
    }
}

/* 将条目移至 MRU 端（链表头） */
static void cache_move_to_head(LRUCache* cache, CacheEntry* e)
{
    if (e == cache->lruHead) return; /* 已在头部 */

    /* 从当前位置摘除 */
    if (e->prev) e->prev->next = e->next;
    if (e->next) e->next->prev = e->prev;
    if (cache->lruTail == e) cache->lruTail = e->prev;

    /* 插入头部 */
    e->prev = NULL;
    e->next = cache->lruHead;
    if (cache->lruHead) cache->lruHead->prev = e;
    cache->lruHead = e;
    if (cache->lruTail == NULL) cache->lruTail = e;
}

/* 淘汰 LRU 端（链表尾） */
static void cache_evict_lru(LRUCache* cache)
{
    if (cache->lruTail == NULL) return;

    CacheEntry* victim = cache->lruTail;

    /* 从哈希表摘除 */
    unsigned int bucket = hash_str(victim->query);
    CacheEntry** pp = &cache->hashTable[bucket];
    while (*pp != NULL && *pp != victim)
        pp = &(*pp)->hashNext;
    if (*pp == victim)
        *pp = victim->hashNext;

    /* 从 LRU 链表摘除 */
    if (victim->prev) victim->prev->next = NULL;
    cache->lruTail = victim->prev;
    if (cache->lruTail == NULL) cache->lruHead = NULL;

    cache_entry_free(victim);
    cache->count--;
}

API void lru_cache_init(int max_size)
{
    lru_cache_free(); /* 先清理旧的 */

    memset(&g_cache, 0, sizeof(g_cache));
    g_cache.maxSize = (max_size > 0) ? max_size : LRU_CACHE_SIZE;
    g_cacheHits = 0;
    g_cacheMisses = 0;
}

API int lru_cache_lookup(const char* query, int top_k, ScoreItem* out_rank)
{
    if (g_cache.maxSize == 0 && g_cache.lruHead == NULL)
        return 0; /* 未初始化 */

    unsigned int bucket = hash_str(query);
    for (CacheEntry* e = g_cache.hashTable[bucket]; e != NULL; e = e->hashNext) {
        if (strcmp(e->query, query) == 0) {
            /* 命中！移至 MRU 端 */
            cache_move_to_head(&g_cache, e);
            g_cacheHits++;
            int n = (e->resultCount < top_k) ? e->resultCount : top_k;
            if (n > 0 && out_rank)
                memcpy(out_rank, e->results, sizeof(ScoreItem) * (size_t)n);
            return n;
        }
    }
    g_cacheMisses++;
    return 0; /* 未命中 */
}

API void lru_cache_insert(const char* query, ScoreItem* results, int result_count)
{
    if (g_cache.maxSize == 0) return; /* 未初始化 */

    /* 1. 如果已存在同 query，先删除旧条目 */
    unsigned int bucket = hash_str(query);
    CacheEntry** pp = &g_cache.hashTable[bucket];
    CacheEntry* exist = NULL;
    while (*pp != NULL) {
        if (strcmp((*pp)->query, query) == 0) {
            exist = *pp;
            *pp = exist->hashNext;
            /* 从 LRU 链表摘除 */
            if (exist->prev) exist->prev->next = exist->next;
            if (exist->next) exist->next->prev = exist->prev;
            if (g_cache.lruHead == exist) g_cache.lruHead = exist->next;
            if (g_cache.lruTail == exist) g_cache.lruTail = exist->prev;
            cache_entry_free(exist);
            g_cache.count--;
            break;
        }
        pp = &(*pp)->hashNext;
    }

    /* 2. 若缓存满，淘汰 LRU */
    while (g_cache.count >= g_cache.maxSize)
        cache_evict_lru(&g_cache);

    /* 3. 插入新条目 */
    CacheEntry* e = cache_entry_new(query, results, result_count);

    /* 加入哈希表（桶头插入） */
    e->hashNext = g_cache.hashTable[bucket];
    g_cache.hashTable[bucket] = e;

    /* 加入 LRU 头部 */
    e->prev = NULL;
    e->next = g_cache.lruHead;
    if (g_cache.lruHead) g_cache.lruHead->prev = e;
    g_cache.lruHead = e;
    if (g_cache.lruTail == NULL) g_cache.lruTail = e;
    g_cache.count++;
}

API void lru_cache_free(void)
{
    for (int i = 0; i < 256; i++) {
        CacheEntry* e = g_cache.hashTable[i];
        while (e != NULL) {
            CacheEntry* next = e->hashNext;
            cache_entry_free(e);
            e = next;
        }
        g_cache.hashTable[i] = NULL;
    }
    g_cache.lruHead = NULL;
    g_cache.lruTail = NULL;
    g_cache.count = 0;
}

API int lru_cache_stats(int* hits, int* misses)
{
    if (hits)   *hits   = g_cacheHits;
    if (misses) *misses = g_cacheMisses;
    return g_cache.count;
}
