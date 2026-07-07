#include "../../public/public.h"
#include "rank_score.h"
#include <math.h>

/* ---------- 内部静态工具函数 ---------- */

static void sort_score_array(ScoreItem* arr, int count)
{
    /* 简单插入排序，按 score 从高到低排列（文档数量不大，足够高效） */
    for (int i = 1; i < count; i++)
    {
        ScoreItem key = arr[i];
        int j = i - 1;
        while (j >= 0 && arr[j].score < key.score)
        {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
}

static WordNode* find_word_node(WordNode* head, const char* word)
{
    for (WordNode* p = head; p != NULL; p = p->next)
    {
        if (strcmp(p->word, word) == 0)
            return p;
    }
    return NULL;
}

/* 判断某个 docId 在 node 的 posList 中是否存在等于 targetPos 的位置 */
static int doc_has_pos(WordNode* node, int docId, int targetPos)
{
    if (node == NULL) return 0;
    for (int i = 0; i < node->posCnt; i++)
    {
        if (node->posList[i].docId == docId && node->posList[i].pos == targetPos)
            return 1;
    }
    return 0;
}

/* ---------- 对外接口实现 ---------- */

/*
 * TF-IDF 评分算法：
 *   TF (词频) = 词在文档中的原始出现次数
 *   IDF (逆文档频率) = log((N+1)/(df+1)) + 1  (平滑版本，避免 df=N 时 log0)
 *   最终得分 = Σ TF × IDF  (所有查询词累加)
 *
 *   短语加分：若查询词位置连续 (如 "search engine" 两词相邻)，
 *   在文档中查找连续出现的位置，匹配成功 +5.0 分
 *
 *   排序：插入排序（O(n²)），文档数量不大时比快排更简洁高效
 */

API int calc_rank_result(char keywords[][MAX_WORD_LEN], int word_cnt, int* pos_arr,
                          WordNode* index_root, int top_k, ScoreItem* out_rank)
{
    if (word_cnt <= 0)
        return 0;

    /* 1. 先扫描索引，找出出现过的最大 docId，从而确定文档总数（用于 IDF 计算） */
    int maxDocId = -1;
    for (WordNode* p = index_root; p != NULL; p = p->next)
    {
        for (int i = 0; i < p->posCnt; i++)
        {
            if (p->posList[i].docId > maxDocId)
                maxDocId = p->posList[i].docId;
        }
    }
    if (maxDocId < 0)
        return 0; /* 索引为空 */

    int totalDocs = maxDocId + 1;
    double* scores = (double*)calloc(totalDocs, sizeof(double));
    int* phraseMatched = (int*)calloc(totalDocs, sizeof(int));
    int* termFreq = (int*)malloc(sizeof(int) * totalDocs);

    /* 记录每个关键词对应的索引节点，供短语检索复用，避免重复查找 */
    WordNode** keyNodes = (WordNode**)malloc(sizeof(WordNode*) * word_cnt);

    /* 2. TF-IDF 累加 */
    for (int k = 0; k < word_cnt; k++)
    {
        WordNode* node = find_word_node(index_root, keywords[k]);
        keyNodes[k] = node;
        if (node == NULL)
            continue;

        memset(termFreq, 0, sizeof(int) * totalDocs);
        for (int i = 0; i < node->posCnt; i++)
        {
            int docId = node->posList[i].docId;
            if (docId >= 0 && docId < totalDocs)
                termFreq[docId]++;
        }

        int distinctDocs = 0;
        for (int d = 0; d < totalDocs; d++)
            if (termFreq[d] > 0) distinctDocs++;

        /* 平滑 IDF：log((N+1)/(df+1)) + 1，避免 df=N 时得分归零 */
        double idf = log((double)(totalDocs + 1) / (double)(distinctDocs + 1)) + 1.0;

        for (int d = 0; d < totalDocs; d++)
        {
            if (termFreq[d] > 0)
                scores[d] += termFreq[d] * idf;
        }
    }

    /* 3. 短语检索加分项：若关键词在查询中的顺序位置是连续的（0,1,2,...），
       则尝试在同一篇文档里寻找这些词是否也连续出现，若找到则加分 */
    int isContiguousQuery = 1;
    for (int k = 1; k < word_cnt; k++)
    {
        if (pos_arr[k] != pos_arr[k - 1] + 1)
        {
            isContiguousQuery = 0;
            break;
        }
    }

    if (isContiguousQuery && word_cnt >= 2 && keyNodes[0] != NULL)
    {
        WordNode* firstNode = keyNodes[0];
        for (int i = 0; i < firstNode->posCnt; i++)
        {
            int docId = firstNode->posList[i].docId;
            int startPos = firstNode->posList[i].pos;
            if (phraseMatched[docId])
                continue;

            int allMatch = 1;
            for (int k = 1; k < word_cnt; k++)
            {
                if (!doc_has_pos(keyNodes[k], docId, startPos + k))
                {
                    allMatch = 0;
                    break;
                }
            }
            if (allMatch)
            {
                phraseMatched[docId] = 1;
                scores[docId] += 5.0; /* 短语完整匹配加分 */
            }
        }
    }

    /* 4. 收集有得分的文档，排序，取前 top_k */
    ScoreItem* all = (ScoreItem*)malloc(sizeof(ScoreItem) * totalDocs);
    int allCnt = 0;
    for (int d = 0; d < totalDocs; d++)
    {
        if (scores[d] > 0.0)
        {
            all[allCnt].docId = d;
            all[allCnt].score = scores[d];
            allCnt++;
        }
    }
    sort_score_array(all, allCnt);

    int outCnt = (allCnt < top_k) ? allCnt : top_k;
    for (int i = 0; i < outCnt; i++)
        out_rank[i] = all[i];

    free(all);
    free(scores);
    free(phraseMatched);
    free(termFreq);
    free(keyNodes);

    return outCnt;
}
