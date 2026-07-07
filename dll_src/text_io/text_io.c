#include "../../public/public.h"
#include "text_io.h"
#include <dirent.h>
#include <ctype.h>

/* ==================== UTF-8 / CJK 辅助函数 ==================== */

/* 返回 UTF-8 字符的字节长度 (1-4)，无效返回 0 */
static int utf8_char_len(const unsigned char* s)
{
    if (*s < 0x80)            return 1;
    if ((*s & 0xE0) == 0xC0)  return 2;
    if ((*s & 0xF0) == 0xE0)  return 3;
    if ((*s & 0xF8) == 0xF0)  return 4;
    return 0;
}

/* 判断 UTF-8 字符（已知长度 len）是否为 CJK 汉字 */
static int is_cjk_char(const unsigned char* s, int len)
{
    if (len == 3) {
        unsigned int cp = ((unsigned int)(s[0] & 0x0F) << 12)
                        | ((unsigned int)(s[1] & 0x3F) << 6)
                        |  (unsigned int)(s[2] & 0x3F);
        return (cp >= 0x4E00 && cp <= 0x9FFF)    /* CJK 统一汉字 */
            || (cp >= 0x3400 && cp <= 0x4DBF)    /* CJK 扩展 A */
            || (cp >= 0xF900 && cp <= 0xFAFF);   /* CJK 兼容汉字 */
    }
    if (len == 4) {
        unsigned int cp = ((unsigned int)(s[0] & 0x07) << 18)
                        | ((unsigned int)(s[1] & 0x3F) << 12)
                        | ((unsigned int)(s[2] & 0x3F) << 6)
                        |  (unsigned int)(s[3] & 0x3F);
        return (cp >= 0x20000 && cp <= 0x2A6DF);  /* CJK 扩展 B */
    }
    return 0;
}

/* 判断是否是 CJK 标点符号 */
static int is_cjk_punct(const unsigned char* s, int len)
{
    if (len == 3) {
        unsigned int cp = ((unsigned int)(s[0] & 0x0F) << 12)
                        | ((unsigned int)(s[1] & 0x3F) << 6)
                        |  (unsigned int)(s[2] & 0x3F);
        return (cp >= 0x3000 && cp <= 0x303F)   /* CJK 标点 */
            || (cp >= 0xFF00 && cp <= 0xFFEF)   /* 全角标点 */
            || (cp >= 0x2000 && cp <= 0x206F)   /* 通用标点 */
            || cp == 0x3001 || cp == 0x3002     /* 、。 */
            || cp == 0xFF0C || cp == 0xFF0E     /* ，． */
            || cp == 0xFF1A || cp == 0xFF1B     /* ：； */
            || cp == 0xFF01 || cp == 0xFF1F;    /* ！？ */
    }
    return 0;
}

/* 判断是否为空白字符（含全角空格 U+3000） */
static int is_space_utf8(const unsigned char* s, int len)
{
    if (len == 1 && isspace((int)s[0])) return 1;
    if (len == 3 && s[0] == 0xE3 && s[1] == 0x80 && s[2] == 0x80) return 1; /* U+3000 */
    return 0;
}

/* ==================== 停用词管理 ==================== */

static char g_stopwords[MAX_STOPWORDS][MAX_STOPWORD_LEN];
static int  g_stopwordCount = 0;

API int load_stopwords(const char* stopwords_path)
{
    g_stopwordCount = 0;
    FILE* fp = fopen(stopwords_path, "r");
    if (fp == NULL) {
        /* 文件不存在时使用内置默认停用词 */
        const char* builtin[] = {
            "the","a","an","is","are","was","were","of","to","in",
            "on","and","or","for","with","at","by","it","as","be",
            "this","that","from","which","he","she","they","we","you","i",
            "\xe7\x9a\x84","\xe4\xba\x86","\xe5\x9c\xa8","\xe6\x98\xaf",
            "\xe6\x88\x91","\xe6\x9c\x89","\xe5\x92\x8c","\xe5\xb0\xb1",
            "\xe4\xb8\x8d","\xe4\xba\xba","\xe9\x83\xbd","\xe4\xb8\x80",
            "\xe4\xb9\x9f","\xe5\xbe\x88","\xe5\x88\xb0","\xe8\xaf\xb4",
            "\xe8\xa6\x81","\xe5\x8e\xbb","\xe4\xbd\xa0","\xe4\xbc\x9a",
            NULL
        };
        for (int i = 0; builtin[i] != NULL && i < MAX_STOPWORDS; i++) {
            strncpy(g_stopwords[i], builtin[i], MAX_STOPWORD_LEN - 1);
            g_stopwords[i][MAX_STOPWORD_LEN - 1] = '\0';
            g_stopwordCount++;
        }
        fprintf(stderr, "[text_io] 停用词文件未找到，使用内置默认停用词 (%d 个)\n", g_stopwordCount);
        return g_stopwordCount;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp) != NULL && g_stopwordCount < MAX_STOPWORDS) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (len == 0 || line[0] == '#') continue;
        char* start = line;
        while (*start == ' ' || *start == '\t') start++;
        if (*start == '\0') continue;

        strncpy(g_stopwords[g_stopwordCount], start, MAX_STOPWORD_LEN - 1);
        g_stopwords[g_stopwordCount][MAX_STOPWORD_LEN - 1] = '\0';
        g_stopwordCount++;
    }
    fclose(fp);
    fprintf(stderr, "[text_io] 已加载 %d 个停用词 (来自 %s)\n", g_stopwordCount, stopwords_path);
    return g_stopwordCount;
}

static int is_stop_word(const char* word)
{
    for (int i = 0; i < g_stopwordCount; i++) {
        if (strcmp(word, g_stopwords[i]) == 0)
            return 1;
    }
    return 0;
}

/* ==================== 内部静态工具函数 ==================== */

static int is_punctuation(char ch)
{
    if (isalnum((unsigned char)ch) || isspace((unsigned char)ch))
        return 0;
    return 1;
}

static void to_lower(char* str)
{
    for (int i = 0; str[i] != '\0'; i++) {
        str[i] = (char)tolower((unsigned char)str[i]);
    }
}

static int has_txt_ext(const char* name)
{
    size_t len = strlen(name);
    if (len < 4) return 0;
    return strcmp(name + len - 4, ".txt") == 0;
}

/* ==================== 中英文混合分词器 ==================== */

/*
 * 分词策略：
 *   - ASCII/拉丁字母：累积成英文单词（空白/标点分隔），转小写，过滤停用词
 *   - CJK 汉字：每个单字作为一个词元 (unigram)，相邻 CJK 字符组成二元组 (bigram)
 *   - 混合文本：在 CJK 和 ASCII 交界处自动切换
 *   - CJK 标点：视为分隔符
 */

/* 添加一个词元到文档的 words 数组（带停用词过滤） */
static int add_token(char** words, int* pCount, const char* token, int tokenLen)
{
    if (tokenLen <= 0 || *pCount >= MAX_DOC_WORDS) return 0;

    /* 构造临时字符串进行停用词检查 */
    char tmp[MAX_WORD_LEN];
    int copyLen = (tokenLen < MAX_WORD_LEN - 1) ? tokenLen : (MAX_WORD_LEN - 1);
    memcpy(tmp, token, (size_t)copyLen);
    tmp[copyLen] = '\0';

    if (is_stop_word(tmp)) return 0;

    words[*pCount] = (char*)malloc((size_t)copyLen + 1);
    memcpy(words[*pCount], token, (size_t)copyLen);
    words[*pCount][copyLen] = '\0';
    (*pCount)++;
    return 1;
}

static void tokenize_doc(DocInfo* doc)
{
    char** words = (char**)malloc(sizeof(char*) * MAX_DOC_WORDS);
    int wordCount = 0;

    const unsigned char* src = (const unsigned char*)doc->content;
    int totalLen = (int)strlen(doc->content);

    /* 英文单词暂存缓冲 */
    char  engBuf[MAX_WORD_LEN];
    int   engLen = 0;

    /* CJK 字符暂存缓冲（用于生成二元组） */
    char  cjkChars[MAX_DOC_WORDS][4];  /* 每个 CJK 字最多 4 字节 UTF-8 */
    int   cjkLens[MAX_DOC_WORDS];
    int   cjkCount = 0;

    /* 辅助宏：提交英文缓冲 */
#define FLUSH_ENG() do { \
    if (engLen > 0) { \
        add_token(words, &wordCount, engBuf, engLen); \
        engLen = 0; \
    } \
} while(0)

    /* 辅助宏：提交 CJK 缓冲（生成 unigram + bigram） */
#define FLUSH_CJK() do { \
    for (int _i = 0; _i < cjkCount && wordCount < MAX_DOC_WORDS; _i++) { \
        add_token(words, &wordCount, cjkChars[_i], cjkLens[_i]); \
    } \
    for (int _i = 0; _i < cjkCount - 1 && wordCount < MAX_DOC_WORDS; _i++) { \
        char bigram[8]; \
        int blen = cjkLens[_i] + cjkLens[_i + 1]; \
        if (blen < (int)sizeof(bigram)) { \
            memcpy(bigram, cjkChars[_i], (size_t)cjkLens[_i]); \
            memcpy(bigram + cjkLens[_i], cjkChars[_i + 1], (size_t)cjkLens[_i + 1]); \
            add_token(words, &wordCount, bigram, blen); \
        } \
    } \
    cjkCount = 0; \
} while(0)

    int pos = 0;
    while (pos < totalLen && wordCount < MAX_DOC_WORDS) {
        int clen = utf8_char_len(src + pos);
        if (clen <= 0) { pos++; continue; }

        if (clen == 1) {
            unsigned char ch = src[pos];
            if (isspace(ch) || is_punctuation((char)ch)) {
                FLUSH_ENG();
                FLUSH_CJK();
            } else {
                /* ASCII 字母/数字 */
                if (cjkCount > 0) FLUSH_CJK();
                if (engLen < MAX_WORD_LEN - 1) {
                    engBuf[engLen++] = (char)tolower(ch);
                }
            }
        } else {
            /* 多字节 UTF-8 字符 */
            if (is_space_utf8(src + pos, clen) || is_cjk_punct(src + pos, clen)) {
                FLUSH_ENG();
                FLUSH_CJK();
            } else if (is_cjk_char(src + pos, clen)) {
                FLUSH_ENG();
                if (cjkCount < MAX_DOC_WORDS && clen <= 4) {
                    memcpy(cjkChars[cjkCount], src + pos, (size_t)clen);
                    cjkChars[cjkCount][clen] = '\0';
                    cjkLens[cjkCount] = clen;
                    cjkCount++;
                }
            } else {
                /* 其他多字节字符（拉丁扩展等） */
                FLUSH_CJK();
                for (int b = 0; b < clen && engLen < MAX_WORD_LEN - 1; b++) {
                    engBuf[engLen++] = (char)tolower(src[pos + b]);
                }
            }
        }
        pos += clen;
    }
    FLUSH_ENG();
    FLUSH_CJK();

#undef FLUSH_ENG
#undef FLUSH_CJK

    doc->words = words;
    doc->wordNum = wordCount;
}

/* ==================== 对外接口实现 ==================== */

API int load_clean_text(const char* folder_path, DocInfo** out_docs, int* total_doc_num)
{
    /* 确保停用词已加载 */
    if (g_stopwordCount == 0) {
        load_stopwords("stopwords.txt");
    }

    DIR* dir = opendir(folder_path);
    if (dir == NULL) {
        fprintf(stderr, "[text_io] 无法打开目录: %s\n", folder_path);
        *out_docs = NULL;
        *total_doc_num = 0;
        return -1;
    }

    struct dirent* entry;
    int fileCount = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (has_txt_ext(entry->d_name)) fileCount++;
    }
    rewinddir(dir);

    DocInfo* docs = (DocInfo*)malloc(sizeof(DocInfo) * (fileCount > 0 ? fileCount : 1));
    int docIdx = 0;

    while ((entry = readdir(dir)) != NULL) {
        if (!has_txt_ext(entry->d_name)) continue;

        char fullPath[MAX_FILENAME_LEN + 512];
        snprintf(fullPath, sizeof(fullPath), "%s/%s", folder_path, entry->d_name);

        FILE* fp = fopen(fullPath, "r");
        if (fp == NULL) continue;

        DocInfo* doc = &docs[docIdx];
        doc->id = docIdx;
        strncpy(doc->fileName, entry->d_name, MAX_FILENAME_LEN - 1);
        doc->fileName[MAX_FILENAME_LEN - 1] = '\0';

        size_t readLen = fread(doc->content, 1, MAX_CONTENT_LEN - 1, fp);
        doc->content[readLen] = '\0';
        fclose(fp);

        /* ASCII 转小写（不影响 UTF-8 多字节序列，因高字节位 > 0x7F 不会被 tolower 修改） */
        to_lower(doc->content);

        /* 中英文混合分词 */
        tokenize_doc(doc);

        docIdx++;
    }
    closedir(dir);

    *out_docs = docs;
    *total_doc_num = docIdx;
    fprintf(stderr, "[text_io] 成功加载 %d 篇文档\n", docIdx);
    return docIdx;
}

API int get_word_suggestions(WordNode* index, const char* prefix,
                              char suggestions[][MAX_WORD_LEN], int max_count)
{
    if (index == NULL || prefix == NULL || max_count <= 0) return 0;

    int prefixLen = (int)strlen(prefix);
    if (prefixLen == 0) return 0;

    int count = 0;
    for (WordNode* p = index; p != NULL && count < max_count; p = p->next) {
        if (strncmp(p->word, prefix, prefixLen) == 0) {
            strncpy(suggestions[count], p->word, MAX_WORD_LEN - 1);
            suggestions[count][MAX_WORD_LEN - 1] = '\0';
            count++;
        }
    }
    return count;
}
