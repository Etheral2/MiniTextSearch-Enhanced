#include "../../public/public.h"
#include "index_build.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

/* ==================== 二进制 I/O 辅助函数 ==================== */

/* 向文件写入一个 uint32 (little-endian) */
static int fwrite_u32(unsigned int val, FILE* fp)
{
    unsigned char buf[4];
    buf[0] = (unsigned char)(val & 0xFF);
    buf[1] = (unsigned char)((val >> 8) & 0xFF);
    buf[2] = (unsigned char)((val >> 16) & 0xFF);
    buf[3] = (unsigned char)((val >> 24) & 0xFF);
    return (int)fwrite(buf, 1, 4, fp) == 4;
}

/* 从内存指针读取一个 uint32 (little-endian)，并推进指针 */
static unsigned int read_u32(const unsigned char** pp)
{
    const unsigned char* p = *pp;
    unsigned int val = (unsigned int)p[0]
                     | ((unsigned int)p[1] << 8)
                     | ((unsigned int)p[2] << 16)
                     | ((unsigned int)p[3] << 24);
    *pp = p + 4;
    return val;
}

/* ==================== 内部静态工具函数 ==================== */

static WordNode* create_word_node(const char* word)
{
    WordNode* node = (WordNode*)malloc(sizeof(WordNode));
    strncpy(node->word, word, MAX_WORD_LEN - 1);
    node->word[MAX_WORD_LEN - 1] = '\0';
    node->posCap = 4;
    node->posList = (WordPos*)malloc(sizeof(WordPos) * node->posCap);
    node->posCnt = 0;
    node->next = NULL;
    return node;
}

static void append_pos(WordNode* node, int docId, int pos)
{
    if (node->posCnt >= node->posCap) {
        node->posCap *= 2;
        node->posList = (WordPos*)realloc(node->posList, sizeof(WordPos) * node->posCap);
    }
    node->posList[node->posCnt].docId = docId;
    node->posList[node->posCnt].pos = pos;
    node->posCnt++;
}

static WordNode* find_word_node(WordNode* head, const char* word)
{
    for (WordNode* p = head; p != NULL; p = p->next) {
        if (strcmp(p->word, word) == 0)
            return p;
    }
    return NULL;
}

/* ==================== 对外接口实现 ==================== */

API int build_save_index(DocInfo* doc_arr, int doc_total, const char* save_path)
{
    WordNode* head = NULL;
    WordNode* tail = NULL;

    /* 1. 构建内存倒排索引链表 */
    for (int d = 0; d < doc_total; d++) {
        DocInfo* doc = &doc_arr[d];
        for (int w = 0; w < doc->wordNum; w++) {
            const char* word = doc->words[w];
            WordNode* node = find_word_node(head, word);
            if (node == NULL) {
                node = create_word_node(word);
                if (head == NULL) { head = node; tail = node; }
                else { tail->next = node; tail = node; }
            }
            append_pos(node, doc->id, w);
        }
    }

    /* 2. 统计单词总数 */
    int wordCount = 0;
    for (WordNode* p = head; p != NULL; p = p->next) wordCount++;

    /* 3. 以二进制格式写入磁盘 */
    FILE* fp = fopen(save_path, "wb");
    if (fp == NULL) {
        fprintf(stderr, "[index_build] 无法写入索引文件: %s\n", save_path);
        /* 释放临时链表 */
        WordNode* p = head;
        while (p != NULL) {
            WordNode* next = p->next;
            free(p->posList);
            free(p);
            p = next;
        }
        return -1;
    }

    /* 头部: magic(4B) + version(4B) + word_count(4B) = 12 bytes */
    fwrite_u32(INDEX_MAGIC, fp);
    fwrite_u32(INDEX_VERSION, fp);
    fwrite_u32((unsigned int)wordCount, fp);

    /* 词项记录 */
    for (WordNode* p = head; p != NULL; p = p->next) {
        unsigned char wordLen = (unsigned char)strlen(p->word);
        if (wordLen > 127) wordLen = 127;  /* 安全截断 */
        fwrite(&wordLen, 1, 1, fp);                    /* 词长度 (1B) */
        fwrite(p->word, 1, wordLen, fp);                /* 词字符串 */
        fwrite_u32((unsigned int)p->posCnt, fp);        /* 位置数量 (4B) */
        /* 写入每个位置: docId(4B) + pos(4B) = 8B each */
        for (int i = 0; i < p->posCnt; i++) {
            fwrite_u32((unsigned int)p->posList[i].docId, fp);
            fwrite_u32((unsigned int)p->posList[i].pos, fp);
        }
    }
    fclose(fp);

    fprintf(stderr, "[index_build] 二进制索引已保存: %s (共 %d 个词项)\n", save_path, wordCount);

    /* 4. 释放临时链表 */
    WordNode* p = head;
    while (p != NULL) {
        WordNode* next = p->next;
        free(p->posList);
        free(p);
        p = next;
    }

    return 0;
}

API WordNode* load_index_file(const char* index_path)
{
#ifdef _WIN32
    /* ======== Windows: 使用 CreateFileMapping + MapViewOfFile ======== */
    HANDLE hFile = CreateFileA(
        index_path,
        GENERIC_READ,
        FILE_SHARE_READ,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if (hFile == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[index_build] 无法打开索引文件: %s\n", index_path);
        return NULL;
    }

    HANDLE hMapping = CreateFileMapping(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (hMapping == NULL) {
        fprintf(stderr, "[index_build] 无法创建文件映射: %s\n", index_path);
        CloseHandle(hFile);
        return NULL;
    }

    const unsigned char* mappedData = (const unsigned char*)MapViewOfFile(
        hMapping, FILE_MAP_READ, 0, 0, 0
    );
    if (mappedData == NULL) {
        fprintf(stderr, "[index_build] 无法映射文件视图: %s\n", index_path);
        CloseHandle(hMapping);
        CloseHandle(hFile);
        return NULL;
    }

    /* 解析二进制索引 */
    const unsigned char* cursor = mappedData;

    unsigned int magic   = read_u32(&cursor);
    unsigned int version = read_u32(&cursor);
    unsigned int wordCount = read_u32(&cursor);

    if (magic != INDEX_MAGIC) {
        fprintf(stderr, "[index_build] 索引文件格式错误 (magic=0x%08X, expected=0x%08X)\n",
                magic, INDEX_MAGIC);
        UnmapViewOfFile(mappedData);
        CloseHandle(hMapping);
        CloseHandle(hFile);
        return NULL;
    }
    if (version != INDEX_VERSION) {
        fprintf(stderr, "[index_build] 索引文件版本不匹配 (got=%u, expected=%u)\n",
                version, INDEX_VERSION);
    }

    fprintf(stderr, "[index_build] mmap 加载索引: %u 个词项\n", wordCount);

    WordNode* head = NULL;
    WordNode* tail = NULL;

    for (unsigned int i = 0; i < wordCount; i++) {
        unsigned char wordLen = *cursor;
        cursor++;

        char word[MAX_WORD_LEN];
        int copyLen = (wordLen < MAX_WORD_LEN - 1) ? (int)wordLen : (MAX_WORD_LEN - 1);
        memcpy(word, cursor, (size_t)copyLen);
        word[copyLen] = '\0';
        cursor += wordLen;

        unsigned int posCnt = read_u32(&cursor);

        WordNode* node = create_word_node(word);
        for (unsigned int j = 0; j < posCnt; j++) {
            int docId = (int)read_u32(&cursor);
            int pos   = (int)read_u32(&cursor);
            append_pos(node, docId, pos);
        }

        if (head == NULL) { head = node; tail = node; }
        else { tail->next = node; tail = node; }
    }

    UnmapViewOfFile(mappedData);
    CloseHandle(hMapping);
    CloseHandle(hFile);

    return head;

#else
    /* ======== POSIX: 使用 mmap ======== */
    int fd = open(index_path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "[index_build] 无法打开索引文件: %s\n", index_path);
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        fprintf(stderr, "[index_build] 无法获取文件大小: %s\n", index_path);
        close(fd);
        return NULL;
    }

    const unsigned char* mappedData = (const unsigned char*)mmap(
        NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0
    );
    if (mappedData == MAP_FAILED) {
        fprintf(stderr, "[index_build] mmap 失败: %s\n", index_path);
        close(fd);
        return NULL;
    }

    const unsigned char* cursor = mappedData;

    unsigned int magic   = read_u32(&cursor);
    unsigned int version = read_u32(&cursor);
    unsigned int wordCount = read_u32(&cursor);

    if (magic != INDEX_MAGIC) {
        fprintf(stderr, "[index_build] 索引文件格式错误 (magic=0x%08X)\n", magic);
        munmap((void*)mappedData, (size_t)st.st_size);
        close(fd);
        return NULL;
    }
    if (version != INDEX_VERSION) {
        fprintf(stderr, "[index_build] 索引文件版本不匹配 (got=%u, expected=%u)\n",
                version, INDEX_VERSION);
    }

    fprintf(stderr, "[index_build] mmap 加载索引: %u 个词项\n", wordCount);

    WordNode* head = NULL;
    WordNode* tail = NULL;

    for (unsigned int i = 0; i < wordCount; i++) {
        unsigned char wordLen = *cursor;
        cursor++;

        char word[MAX_WORD_LEN];
        int copyLen = (wordLen < MAX_WORD_LEN - 1) ? (int)wordLen : (MAX_WORD_LEN - 1);
        memcpy(word, cursor, (size_t)copyLen);
        word[copyLen] = '\0';
        cursor += wordLen;

        unsigned int posCnt = read_u32(&cursor);

        WordNode* node = create_word_node(word);
        for (unsigned int j = 0; j < posCnt; j++) {
            int docId = (int)read_u32(&cursor);
            int pos   = (int)read_u32(&cursor);
            append_pos(node, docId, pos);
        }

        if (head == NULL) { head = node; tail = node; }
        else { tail->next = node; tail = node; }
    }

    munmap((void*)mappedData, (size_t)st.st_size);
    close(fd);

    return head;
#endif
}
