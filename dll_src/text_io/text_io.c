#include "../../public/public.h"
#include "text_io.h"
#include <dirent.h>
#include <ctype.h>

/* my_memmem 实现（MinGW 不提供此函数） */
static void* my_memmem(const void* haystack, size_t haystackLen,
                       const void* needle, size_t needleLen)
{
	if (needleLen == 0) return (void*)haystack;
	if (haystackLen < needleLen) return NULL;
	const unsigned char* h = (const unsigned char*)haystack;
	const unsigned char* end = h + haystackLen - needleLen;
	unsigned char first = *(const unsigned char*)needle;
	while (h <= end) {
		if (*h == first && memcmp(h, needle, needleLen) == 0)
			return (void*)h;
		h++;
	}
	return NULL;
}

/* my_strcasecmp 实现（跨平台兼容） */
static int my_strcasecmp(const char* a, const char* b)
{
	while (*a && *b) {
		char ca = (char)tolower((unsigned char)*a);
		char cb = (char)tolower((unsigned char)*b);
		if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
		a++; b++;
	}
	return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* my_strncasecmp 实现 */
static int my_strncasecmp(const char* a, const char* b, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++) {
		if (!a[i] && !b[i]) return 0;
		char ca = (char)tolower((unsigned char)a[i]);
		char cb = (char)tolower((unsigned char)b[i]);
		if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
		if (!a[i]) return -1;
		if (!b[i]) return 1;
	}
	return 0;
}

/* ==================== 最小化 inflate（RFC 1950/1951） ==================== */
/*
 * 内嵌轻量版 inflate 解压缩算法，用于 PDF FlateDecode 和 DOCX ZIP 解压。
 *
 * 算法核心流程：
 *   1. 读取块头（bfinal + btype），识别三种块类型
 *   2. type=0: 未压缩块，直接拷贝
 *   3. type=1: 固定 Huffman 码表（RFC 1951 3.2.6 预定义）
 *   4. type=2: 动态 Huffman 码表，先解码码长序列再重建树
 *   5. 每个 LZ77 码字解码为 (length, distance) 对，回退拷贝
 *
 * zlib 包装 (tinf_zlib_uncompress): 2 字节头 + 原始 deflate + 4 字节 Adler32
 *
 * 参考: RFC 1950 (zlib), RFC 1951 (deflate) */

#define TINF_OK      0
#define TINF_ERR    -1
#define TINF_NEED    1

typedef struct {
	unsigned short counts[16];   /* 每种码长的符号个数 */
	unsigned short symbols[288]; /* 按码长排序的符号列表 */
} TinfTree;

/* bit 读取器 */
typedef struct {
	const unsigned char* data;
	int len;
	int bitPos; /* 当前位位置 (0 = MSB of byte) */
} TinfBits;

static void tinf_bits_init(TinfBits* b, const void* data, int len) {
	b->data = (const unsigned char*)data;
	b->len = len;
	b->bitPos = 0;
}

/* 读取 n 位 (n <= 16)，高位在前 */
static int tinf_bits_read(TinfBits* b, int n, unsigned int* val) {
	unsigned int v = 0;
	if (b->len < 0) return TINF_ERR;
	while (n > 0) {
		if (b->len == 0 && b->bitPos == 0) return TINF_ERR;
		int avail = 8 - b->bitPos;
		int take = (n < avail) ? n : avail;
		v = (v << take) | (((unsigned int)b->data[0] >> (avail - take)) & ((1u << take) - 1));
		b->bitPos += take;
		n -= take;
		if (b->bitPos >= 8) {
			b->bitPos = 0;
			b->data++;
			b->len--;
		}
	}
	*val = v;
	return TINF_OK;
}

/* 跳过到字节边界 */
static void tinf_bits_align(TinfBits* b) {
	if (b->bitPos > 0) {
		b->bitPos = 0;
		b->data++;
		b->len--;
	}
}

/* Huffman 树构建：从 codeLengths[0..n-1] 构建树 */
static int tinf_tree_build(TinfTree* t, const unsigned short* lengths, int n) {
	unsigned short offs[16];
	int i;

	memset(t->counts, 0, sizeof(t->counts));
	for (i = 0; i < n; i++) {
		if (lengths[i] > 15) return TINF_ERR;
		t->counts[lengths[i]]++;
	}
	t->counts[0] = 0; /* 码长为0的符号不使用 */

	offs[1] = 0;
	for (i = 2; i <= 15; i++) {
		offs[i] = offs[i - 1] + t->counts[i - 1];
	}
	for (i = 0; i < n; i++) {
		if (lengths[i] > 0) {
			t->symbols[offs[lengths[i]]] = (unsigned short)i;
			offs[lengths[i]]++;
		}
	}
	return TINF_OK;
}

/* 用给定树解码一个符号 */
static int tinf_decode_one(TinfBits* b, const TinfTree* t, unsigned int* sym) {
	unsigned int code = 0;
	int first = 0;
	int idx = 0;

	for (int len = 1; len <= 15; len++) {
		unsigned int bit;
		if (tinf_bits_read(b, 1, &bit) != TINF_OK) return TINF_ERR;
		code = (code << 1) | (bit & 1);
		int count = t->counts[len];
		if (code - first < (unsigned int)count) {
			*sym = t->symbols[idx + (code - first)];
			return TINF_OK;
		}
		first = (first + count) << 1;
		idx += count;
	}
	return TINF_ERR;
}

/* 长度/距离基础值表 (RFC 1951) */
static const unsigned short tinf_lengBase[] = {
	3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
	35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const unsigned short tinf_lengExtra[] = {
	0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
	3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const unsigned short tinf_distBase[] = {
	1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
	257,385,513,769,1025,1537,2049,3073,4097,6145,
	8193,12289,16385,24577
};
static const unsigned short tinf_distExtra[] = {
	0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
	7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

/* 解压函数 (raw deflate RFC 1951) */
static int tinf_uncompress_raw(void* dest, int* destLen,
                                const void* source, int sourceLen) {
	TinfBits b;
	unsigned char* out = (unsigned char*)dest;
	int outMax = *destLen;
	int outPos = 0;
	int final;

	tinf_bits_init(&b, source, sourceLen);

	do {
		unsigned int bfinal, btype;
		if (tinf_bits_read(&b, 1, &bfinal) != TINF_OK) break;
		final = (int)bfinal;
		if (tinf_bits_read(&b, 2, &btype) != TINF_OK) break;

		if (btype == 0) {
			/* 未压缩块 */
			tinf_bits_align(&b);
			if (b.len < 4) break;
			unsigned int len = (unsigned int)b.data[0] | ((unsigned int)b.data[1] << 8);
			/* unsigned int nlen = (unsigned int)b.data[2] | ((unsigned int)b.data[3] << 8); */
			b.data += 4; b.len -= 4;
			if (len > (unsigned int)(b.len)) len = (unsigned int)b.len;
			if (outPos + (int)len > outMax) len = (unsigned int)(outMax - outPos);
			memcpy(out + outPos, b.data, len);
			outPos += (int)len;
			b.data += len; b.len -= len;
		} else if (btype == 1 || btype == 2) {
			TinfTree lt, dt;
			if (btype == 1) {
				/* 固定 Huffman */
				unsigned short ll[288], dl[32];
				int i;
				for (i = 0; i < 144; i++) ll[i] = 8;
				for (i = 144; i < 256; i++) ll[i] = 9;
				for (i = 256; i < 280; i++) ll[i] = 7;
				for (i = 280; i < 288; i++) ll[i] = 8;
				for (i = 0; i < 32; i++) dl[i] = 5;
				if (tinf_tree_build(&lt, ll, 288) != TINF_OK) break;
				if (tinf_tree_build(&dt, dl, 32) != TINF_OK) break;
			} else {
				/* 动态 Huffman */
				unsigned int hlit, hdist, hclen;
				unsigned short lengths[320]; /* 288 + 32 */
				unsigned short clCodes[19];
				int i;

				if (tinf_bits_read(&b, 5, &hlit) != TINF_OK) break;
				if (tinf_bits_read(&b, 5, &hdist) != TINF_OK) break;
				if (tinf_bits_read(&b, 4, &hclen) != TINF_OK) break;
				hlit += 257; hdist += 1; hclen += 4;

				const int clOrder[] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
				memset(clCodes, 0, sizeof(clCodes));
				for (i = 0; i < (int)hclen; i++) {
					unsigned int v;
					if (tinf_bits_read(&b, 3, &v) != TINF_OK) goto dyn_err;
					clCodes[clOrder[i]] = (unsigned short)v;
				}
				TinfTree clt;
				if (tinf_tree_build(&clt, clCodes, 19) != TINF_OK) break;

				memset(lengths, 0, sizeof(lengths));
				int total = (int)(hlit + hdist);
				for (i = 0; i < total; ) {
					unsigned int sym;
					if (tinf_decode_one(&b, &clt, &sym) != TINF_OK) goto dyn_err;
					if (sym < 16) {
						lengths[i++] = (unsigned short)sym;
					} else {
						int repeat = 0;
						unsigned short val = 0;
						if (sym == 16) {
							if (i == 0) goto dyn_err;
							unsigned int r;
							if (tinf_bits_read(&b, 2, &r) != TINF_OK) goto dyn_err;
							repeat = (int)r + 3;
							val = lengths[i - 1];
						} else if (sym == 17) {
							unsigned int r;
							if (tinf_bits_read(&b, 3, &r) != TINF_OK) goto dyn_err;
							repeat = (int)r + 3;
							val = 0;
						} else { /* 18 */
							unsigned int r;
							if (tinf_bits_read(&b, 7, &r) != TINF_OK) goto dyn_err;
							repeat = (int)r + 11;
							val = 0;
						}
						if (i + repeat > total) goto dyn_err;
						while (repeat-- > 0) lengths[i++] = val;
					}
				}
				if (tinf_tree_build(&lt, lengths, (int)hlit) != TINF_OK) break;
				if (tinf_tree_build(&dt, lengths + hlit, (int)hdist) != TINF_OK) break;
				goto skip_dyn_err;
			dyn_err:
				break;
			skip_dyn_err: ;
			}

			/* 解码 */
			for (;;) {
				unsigned int sym;
				if (tinf_decode_one(&b, &lt, &sym) != TINF_OK) break;
				if (sym == 256) break; /* 块结束 */
				if (sym < 256) {
					if (outPos >= outMax) break;
					out[outPos++] = (unsigned char)sym;
				} else {
					/* 长度/距离对 */
					int lenIdx = (int)sym - 257;
					if (lenIdx < 0 || lenIdx > 28) break;
					unsigned int extra;
					int length = (int)tinf_lengBase[lenIdx];
					int extraBits = tinf_lengExtra[lenIdx];
					if (extraBits > 0) {
						if (tinf_bits_read(&b, extraBits, &extra) != TINF_OK) break;
						length += (int)extra;
					}
					unsigned int distSym;
					if (tinf_decode_one(&b, &dt, &distSym) != TINF_OK) break;
					if (distSym > 29) break;
					int dist = (int)tinf_distBase[distSym];
					int distExtraBits = tinf_distExtra[distSym];
					if (distExtraBits > 0) {
						if (tinf_bits_read(&b, distExtraBits, &extra) != TINF_OK) break;
						dist += (int)extra;
					}
					if (dist > outPos) break;
					int srcPos = outPos - dist;
					int copyLen = length;
					if (outPos + copyLen > outMax) copyLen = outMax - outPos;
					for (int j = 0; j < copyLen; j++) {
						out[outPos++] = out[srcPos + j];
					}
					if (outPos >= outMax) break;
				}
			}
		} else {
			break; /* 保留块类型，报错 */
		}
	} while (!final && outPos < outMax);

	*destLen = outPos;
	return TINF_OK;
}

/* zlib 包装解压 (RFC 1950: 2字节头 + raw deflate + 4字节 Adler32 尾) */
static int tinf_zlib_uncompress(void* dest, int* destLen,
                                 const void* source, int sourceLen) {
	const unsigned char* src = (const unsigned char*)source;
	if (sourceLen < 6) return TINF_ERR;

	/* CMF: bits 0-3 = CM (8=deflate), bits 4-7 = CINFO */
	unsigned int cm = src[0] & 0x0F;
	/* unsigned int cinfo = (src[0] >> 4) & 0x0F; */
	if (cm != 8) return TINF_ERR; /* 仅支持 deflate */

	/* FLG: check header */
	unsigned int flg = src[1];
	(void)flg;
	/* 跳过预设字典：FLG bit 5 */
	if (flg & 0x20) {
		/* 预设字典存在，跳过 4 字节 dictid */
		return tinf_uncompress_raw(dest, destLen, src + 6, sourceLen - 6 - 4);
	}
	return tinf_uncompress_raw(dest, destLen, src + 2, sourceLen - 2 - 4);
}

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
		fprintf(stderr, "[text_io] stopword file not found, using %d built-in stopwords\n", g_stopwordCount);
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
	fprintf(stderr, "[text_io] Loaded %d stopwords from %s\n", g_stopwordCount, stopwords_path);
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

/* ==================== 文件类型检测 ==================== */

typedef enum {
	FILE_TYPE_UNKNOWN = 0,
	FILE_TYPE_TXT,
	FILE_TYPE_HTML,
	FILE_TYPE_PDF,
	FILE_TYPE_DOCX
} FileType;

static int has_ext(const char* name, const char* ext)
{
	size_t nameLen = strlen(name);
	size_t extLen = strlen(ext);
	if (nameLen < extLen) return 0;
	return my_strcasecmp(name + nameLen - extLen, ext) == 0;
}

static FileType get_file_type(const char* name)
{
	if (has_ext(name, ".txt"))  return FILE_TYPE_TXT;
	if (has_ext(name, ".htm") || has_ext(name, ".html")) return FILE_TYPE_HTML;
	if (has_ext(name, ".pdf"))  return FILE_TYPE_PDF;
	if (has_ext(name, ".docx")) return FILE_TYPE_DOCX;
	return FILE_TYPE_UNKNOWN;
}

static int is_supported_ext(const char* name)
{
	return get_file_type(name) != FILE_TYPE_UNKNOWN;
}

/* ==================== 二进制文件读取 ==================== */

/* 以二进制方式读取整个文件，返回 malloc 分配的缓冲区，*len 为长度 */
static char* read_file_binary(const char* path, int* len)
{
	FILE* fp = fopen(path, "rb");
	if (!fp) return NULL;

	fseek(fp, 0, SEEK_END);
	long fsize = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	if (fsize <= 0 || fsize > 50 * 1024 * 1024) { /* 限制 50 MB */
		fclose(fp);
		return NULL;
	}

	char* buf = (char*)malloc((size_t)fsize);
	if (!buf) { fclose(fp); return NULL; }

	*len = (int)fread(buf, 1, (size_t)fsize, fp);
	fclose(fp);
	return buf;
}

/* ==================== HTML 解析器 ==================== */

/* 解码 HTML 实体：&amp; &lt; &gt; &quot; &#39; &nbsp; &#NNN; &#xNNN; */
static int decode_html_entity(const char* src, int srcLen, char* outChar)
{
	if (srcLen < 2 || src[0] != '&') return 0;

	const char* p = src + 1;
	int remain = srcLen - 1;

	if (remain >= 3 && strncmp(p, "amp;", 4) == 0)      { *outChar = '&';  return 5; }
	if (remain >= 3 && strncmp(p, "lt;", 3) == 0)       { *outChar = '<';  return 4; }
	if (remain >= 3 && strncmp(p, "gt;", 3) == 0)       { *outChar = '>';  return 4; }
	if (remain >= 5 && strncmp(p, "quot;", 5) == 0)     { *outChar = '"';  return 6; }
	if (remain >= 5 && strncmp(p, "apos;", 5) == 0)     { *outChar = '\''; return 6; }
	if (remain >= 5 && strncmp(p, "nbsp;", 5) == 0)     { *outChar = ' ';  return 6; }

	/* &#NNN; */
	if (p[0] == '#') {
		int num = 0;
		int i = 1;
		if (p[1] == 'x' || p[1] == 'X') {
			/* &#xNNN; */
			i = 2;
			while (i < remain && p[i] != ';') {
				char c = (char)tolower((unsigned char)p[i]);
				if (c >= '0' && c <= '9')      num = num * 16 + (c - '0');
				else if (c >= 'a' && c <= 'f') num = num * 16 + (c - 'a' + 10);
				else break;
				i++;
			}
		} else {
			while (i < remain && p[i] >= '0' && p[i] <= '9') {
				num = num * 10 + (p[i] - '0');
				i++;
			}
		}
		if (p[i] == ';' && num > 0 && num <= 255) {
			*outChar = (char)num;
			return 1 + i + 1; /* & + content + ; */
		}
	}

	return 0; /* 未识别的实体，保留原文 */
}

/* 将 HTML 转为纯文本 */
static int parse_html_to_text(const char* html, int htmlLen, char* out, int maxOut)
{
	int outPos = 0;
	int inTag = 0;        /* 在标签内 */
	int inScript = 0;     /* 在 <script> 内 */
	int inStyle = 0;      /* 在 <style> 内 */
	int lastWasSpace = 0; /* 用于压缩连续空白 */

	const char* blockTags[] = {
		"p","/p","br","h1","/h1","h2","/h2","h3","/h3","h4","/h4","h5","/h5","h6","/h6",
		"div","/div","li","/li","tr","/tr","td","/td","th","/th","hr","/hr",
		"section","/section","article","/article","header","/header","footer","/footer",
		"pre","/pre","blockquote","/blockquote","ol","/ol","ul","/ul","table","/table",
		"title","/title","meta","link",
		NULL
	};
	const int numBlockTags = 35; /* 非 NULL 的个数 */

#define HTML_OUT(c) do { \
	if (outPos < maxOut - 1) { out[outPos++] = (c); lastWasSpace = ((c) == '\n' || (c) == ' '); } \
} while(0)

	int i = 0;
	while (i < htmlLen && outPos < maxOut - 1) {
		if (inScript || inStyle) {
			/* 寻找 </script> 或 </style> */
			if (html[i] == '<' && i + 1 < htmlLen && html[i+1] == '/') {
				const char* afterSlash = html + i + 2;
				int remain = htmlLen - i - 2;
				if (inScript && remain >= 7 && my_strncasecmp(afterSlash, "script>", 7) == 0) {
					inScript = 0; i += 9; continue;
				}
				if (inStyle && remain >= 6 && my_strncasecmp(afterSlash, "style>", 6) == 0) {
					inStyle = 0; i += 8; continue;
				}
			}
			i++;
			continue;
		}

		if (html[i] == '<') {
			inTag = 1;
			/* 检查是否是 <script> 或 <style> */
			int remain = htmlLen - i - 1;
			if (remain >= 6 && my_strncasecmp(html + i, "<script", 7) == 0
			    && (html[i+7] == '>' || html[i+7] == ' ')) {
				inScript = 1; inTag = 0; i += 7;
				/* 跳过直到 > */
				while (i < htmlLen && html[i] != '>') i++;
				i++;
				continue;
			}
			if (remain >= 5 && my_strncasecmp(html + i, "<style", 6) == 0
			    && (html[i+6] == '>' || html[i+6] == ' ')) {
				inStyle = 1; inTag = 0; i += 6;
				while (i < htmlLen && html[i] != '>') i++;
				i++;
				continue;
			}
			/* 检查是否是块级标签，用于插入换行 */
			if (html[i+1] != '/') {
				/* 开始标签 */
				for (int t = 0; t < numBlockTags; t++) {
					int tlen = (int)strlen(blockTags[t]);
					if (tlen > 0 && remain >= tlen
					    && my_strncasecmp(html + i + 1, blockTags[t], tlen) == 0
					    && (html[i + 1 + tlen] == '>' || html[i + 1 + tlen] == ' ')) {
						HTML_OUT('\n');
						break;
					}
				}
			} else {
				/* 结束标签 */
				for (int t = 0; t < numBlockTags; t++) {
					int tlen = (int)strlen(blockTags[t]);
					if (tlen > 1 && blockTags[t][0] == '/' && remain >= tlen - 1
					    && my_strncasecmp(html + i + 2, blockTags[t] + 1, tlen - 1) == 0) {
						HTML_OUT('\n');
						break;
					}
				}
			}
			i++;
			continue;
		}

		if (html[i] == '>') {
			inTag = 0;
			i++;
			continue;
		}

		if (inTag) {
			i++;
			continue;
		}

		/* 文本内容 */
		if (html[i] == '&') {
			char entityChar;
			int consumed = decode_html_entity(html + i, htmlLen - i, &entityChar);
			if (consumed > 0) {
				HTML_OUT(entityChar);
				i += consumed;
				continue;
			}
		}

		if (html[i] == '\n' || html[i] == '\r' || html[i] == '\t') {
			if (!lastWasSpace) HTML_OUT(' ');
			i++;
			continue;
		}

		HTML_OUT(html[i]);
		i++;
	}

#undef HTML_OUT
	out[outPos] = '\0';
	return outPos;
}

/* ==================== PDF 文本抽取器 ==================== */

/*
 * 最小化 PDF 文本抽取：
 *   1. 找到各对象中的流 (stream...endstream)
 *   2. 若 /Filter /FlateDecode，用内嵌 inflate 解压
 *   3. 在流数据中查找 BT...ET 文本块
 *   4. 解析 Tj / TJ 操作数，提取字符串
 */

/* 解析 PDF 字面字符串: (Hello \(World\)\n) → 提取内容 */
static int pdf_parse_string(const char* data, int len, char* out, int maxOut)
{
	if (len < 2 || data[0] != '(') return 0;
	int depth = 1;
	int pos = 1;
	int outPos = 0;

	while (pos < len && depth > 0 && outPos < maxOut - 1) {
		char c = data[pos];
		if (c == '\\') {
			pos++;
			if (pos >= len) break;
			c = data[pos];
			switch (c) {
				case 'n':  out[outPos++] = '\n'; break;
				case 'r':  out[outPos++] = '\r'; break;
				case 't':  out[outPos++] = '\t'; break;
				case 'b':  out[outPos++] = '\b'; break;
				case 'f':  out[outPos++] = '\f'; break;
				case '(':
				case ')':
				case '\\': out[outPos++] = c; break;
				default:
					/* 八进制转义 \ddd */
					if (c >= '0' && c <= '7') {
						int oct = c - '0';
						if (pos+1 < len && data[pos+1] >= '0' && data[pos+1] <= '7') {
							oct = oct * 8 + (data[++pos] - '0');
						}
						if (pos+1 < len && data[pos+1] >= '0' && data[pos+1] <= '7') {
							oct = oct * 8 + (data[++pos] - '0');
						}
						out[outPos++] = (char)(oct & 0xFF);
					} else {
						out[outPos++] = c;
					}
					break;
			}
		} else if (c == '(') {
			depth++;
			if (depth > 1) out[outPos++] = c;
		} else if (c == ')') {
			depth--;
			if (depth > 0) out[outPos++] = c;
		} else if (c == '\r' || c == '\n') {
			/* PDF 字符串中换行视为单个空格 */
			out[outPos++] = ' ';
		} else {
			out[outPos++] = c;
		}
		pos++;
	}
	out[outPos] = '\0';
	return outPos;
}

/* 从 PDF 数据中抽取文本 */
static int pdf_extract_text(const char* pdfData, int pdfLen, char* out, int maxOut)
{
	int outPos = 0;
	const char* p = pdfData;
	const char* end = pdfData + pdfLen;

	/* 辅助函数：在给定范围内查找 BT...ET 块 */
	while (outPos < maxOut - 1) {
		/* 查找流 */
		const char* streamData = NULL;

		/* 扫描下一个对象 */
		while (p < end - 10) {
			/* 找 "stream\n" 或 "stream\r\n" */
			const char* s = (const char*)my_memmem(p, end - p, "stream", 6);
			if (!s) { p = end; break; }

			/* stream 后必须是 \n 或 \r\n */
			const char* dataStart = s + 6;
			if (dataStart < end && *dataStart == '\r') dataStart++;
			if (dataStart < end && *dataStart == '\n') {
				dataStart++;
				streamData = dataStart;
			} else {
				p = s + 6;
				continue;
			}

			/* 在 stream 前面查找字典 <<...>> 获取 /Length */
			int declaredLen = -1;
			int filterFlate = 0;
			{
				/* 从 stream 前面反向找 << */
				const char* dictEnd = s;
				const char* dictStart = NULL;
				for (const char* d = s - 2; d > p && d > pdfData + 2; d--) {
					if (d[0] == '<' && d[1] == '<') { dictStart = d; break; }
				}
				if (dictStart != NULL) {
					/* 检查 /Filter /FlateDecode */
					const char* fpos = (const char*)my_memmem(dictStart, dictEnd - dictStart,
					                                       "/FlateDecode", 12);
					if (fpos) filterFlate = 1;
					/* 获取 /Length */
					const char* lpos = (const char*)my_memmem(dictStart, dictEnd - dictStart,
					                                       "/Length", 7);
					if (lpos) {
						const char* numStart = lpos + 7;
						while (numStart < dictEnd && (*numStart == ' ' || *numStart == '\t'
						       || *numStart == '\r' || *numStart == '\n'))
							numStart++;
						if (numStart < dictEnd) {
							declaredLen = 0;
							while (numStart < dictEnd && isdigit((unsigned char)*numStart)) {
								declaredLen = declaredLen * 10 + (*numStart - '0');
								numStart++;
							}
						}
					}
				}
			}

			/* 找 endstream */
			const char* es = (const char*)my_memmem(streamData, end - streamData,
			                                     "endstream", 9);
			if (!es) { p = s + 6; continue; }

			/* 确定流数据范围 */
			int dataLen;
			if (declaredLen > 0 && streamData + declaredLen <= es) {
				dataLen = declaredLen;
			} else {
				dataLen = (int)(es - streamData);
			}
			p = es + 9;

			/* 如果流被压缩，解压 */
			char* decompBuf = NULL;
			const char* textData = streamData;
			int textLen = dataLen;

			if (filterFlate && dataLen > 2) {
				decompBuf = (char*)malloc((size_t)dataLen * 20); /* 假设最大压缩比 20:1 */
				if (decompBuf) {
					int decompLen = dataLen * 20;
					if (tinf_zlib_uncompress(decompBuf, &decompLen,
					                         streamData, dataLen) == TINF_OK && decompLen > 0) {
						textData = decompBuf;
						textLen = decompLen;
						/* 确保以空字符结尾 */
						decompBuf[decompLen] = '\0';
					} else {
						free(decompBuf); decompBuf = NULL;
					}
				}
			}

			/* 在流数据中查找 BT...ET 文本块 */
			{
				const char* tp = textData;
				const char* tEnd = textData + textLen;
				while (tp < tEnd - 10) {
					/* 查找 BT */
					const char* bt = NULL;
					for (const char* scan = tp; scan < tEnd - 2; scan++) {
						if (scan[0] == 'B' && scan[1] == 'T'
						    && (scan[2] == '\n' || scan[2] == '\r' || scan[2] == ' '
						        || scan[2] == '\t' || (scan == textData || !isalnum((unsigned char)scan[-1])))) {
							bt = scan;
							break;
						}
					}
					if (!bt) break;

					/* 查找 ET */
					const char* et = NULL;
					for (const char* scan = bt + 2; scan < tEnd - 2; scan++) {
						if (scan[0] == 'E' && scan[1] == 'T'
						    && (scan[2] == '\n' || scan[2] == '\r' || scan[2] == ' '
						        || scan[2] == '\t')) {
							et = scan;
							break;
						}
					}
					if (!et) break;

					/* 处理 BT...ET 块内的文本操作符 */
					const char* block = bt + 2;
					int blockLen = (int)(et - block);

					for (int bi = 0; bi < blockLen; bi++) {
						/* 查找 '(' */
						if (block[bi] == '(') {
							char strBuf[2048];
							int slen = pdf_parse_string(block + bi,
							                            blockLen - bi, strBuf, sizeof(strBuf));
							if (slen > 0) {
								bi += slen + 1; /* 跳过字符串 + () */
								/* 查找后续的 Tj 或 TJ */
								int ahead = bi;
								while (ahead < blockLen && (block[ahead] == ' '
								       || block[ahead] == '\t' || block[ahead] == '\r'
								       || block[ahead] == '\n'))
									ahead++;
								if (ahead + 2 <= blockLen) {
									if ((block[ahead] == 'T' && block[ahead+1] == 'j')
									    || (block[ahead] == 'T' && block[ahead+1] == 'J')) {
										if (outPos > 0 && outPos < maxOut - 1
										    && out[outPos-1] != ' ' && out[outPos-1] != '\n')
											out[outPos++] = ' ';
										for (int si = 0; si < slen && outPos < maxOut - 1; si++)
											out[outPos++] = strBuf[si];
										out[outPos++] = ' ';
									}
								}
							}
						}
						/* 查找 TJ 数组：[ (str1) num (str2) ... ] TJ */
						if (block[bi] == '[') {
							/* 扫描到 ] 并提取其中的字符串 */
							int aj = bi + 1;
							while (aj < blockLen - 1 && block[aj] != ']') {
								if (block[aj] == '(') {
									char strBuf[2048];
									int slen = pdf_parse_string(block + aj,
									                            blockLen - aj, strBuf, sizeof(strBuf));
									if (slen > 0) {
										if (outPos > 0 && outPos < maxOut - 1
										    && out[outPos-1] != ' ' && out[outPos-1] != '\n')
											out[outPos++] = ' ';
										for (int si = 0; si < slen && outPos < maxOut - 1; si++)
											out[outPos++] = strBuf[si];
										aj += slen + 1;
									} else { aj++; }
								} else {
									aj++;
								}
							}
							bi = aj;
						}
					}
					tp = et + 2;
				}
			}

			free(decompBuf);
		}
		break; /* 只处理一轮 — 所有流 */
	}

	/* 回退：如果没找到 BT/ET 块，尝试简单的字面字符串提取 */
	if (outPos == 0) {
		const char* scan = pdfData;
		const char* scanEnd = pdfData + pdfLen;
		while (scan < scanEnd - 2) {
			if (scan[0] == '(' && (scan == pdfData || scan[-1] != '\\')) {
				char strBuf[4096];
				int slen = pdf_parse_string(scan, (int)(scanEnd - scan), strBuf, sizeof(strBuf));
				if (slen > 3 && outPos < maxOut - 1) { /* 只取较长字符串，跳过短代码 */
					for (int si = 0; si < slen && outPos < maxOut - 1; si++)
						out[outPos++] = strBuf[si];
					out[outPos++] = ' ';
					scan += slen + 1;
				} else {
					scan++;
				}
			} else {
				scan++;
			}
		}
	}

	out[outPos] = '\0';
	return outPos;
}

/* ==================== DOCX / ZIP 解析器 ==================== */

/* 从 ZIP 中提取 word/document.xml 并转为纯文本 */
static int parse_docx_to_text(const char* filepath, char* out, int maxOut)
{
	FILE* fp = fopen(filepath, "rb");
	if (!fp) return 0;

	/* 读入整个文件 */
	fseek(fp, 0, SEEK_END);
	long fsize = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (fsize <= 0 || fsize > 30 * 1024 * 1024) { fclose(fp); return 0; }

	unsigned char* zipData = (unsigned char*)malloc((size_t)fsize);
	if (!zipData) { fclose(fp); return 0; }
	int zipLen = (int)fread(zipData, 1, (size_t)fsize, fp);
	fclose(fp);

	if (zipLen < 66) { free(zipData); return 0; }

	/* 查找 EOCD: 签名 PK\x05\x06 (0x06054b50) */
	int eocdOff = -1;
	for (int i = zipLen - 22; i >= 0 && i > zipLen - 66000; i--) {
		if (zipData[i] == 0x50 && zipData[i+1] == 0x4B
		    && zipData[i+2] == 0x05 && zipData[i+3] == 0x06) {
			eocdOff = i;
			break;
		}
	}
	if (eocdOff < 0) { free(zipData); return 0; }

	/* 读取中央目录偏移和大小 */
	unsigned int cdSize = (unsigned int)zipData[eocdOff + 12]
	                    | ((unsigned int)zipData[eocdOff + 13] << 8)
	                    | ((unsigned int)zipData[eocdOff + 14] << 16)
	                    | ((unsigned int)zipData[eocdOff + 15] << 24);
	unsigned int cdOff  = (unsigned int)zipData[eocdOff + 16]
	                    | ((unsigned int)zipData[eocdOff + 17] << 8)
	                    | ((unsigned int)zipData[eocdOff + 18] << 16)
	                    | ((unsigned int)zipData[eocdOff + 19] << 24);

	if (cdOff + cdSize > (unsigned int)zipLen) { free(zipData); return 0; }

	/* 扫描中央目录找 word/document.xml */
	int foundOff = -1;    /* 本地文件头偏移 */
	int foundCSize = 0;   /* 压缩后大小 */
	int foundUSize = 0;   /* 原始大小 */
	int foundMethod = 0;  /* 压缩方法 */

	int pos = (int)cdOff;
	int cdEnd = pos + (int)cdSize;
	while (pos < cdEnd - 46) {
		if (zipData[pos] != 0x50 || zipData[pos+1] != 0x4B
		    || zipData[pos+2] != 0x01 || zipData[pos+3] != 0x02)
			break; /* 不是中央目录条目 */

		int compMethod  = (int)zipData[pos + 10] | ((int)zipData[pos + 11] << 8);
		unsigned int cSize = (unsigned int)zipData[pos + 20]
		                   | ((unsigned int)zipData[pos + 21] << 8)
		                   | ((unsigned int)zipData[pos + 22] << 16)
		                   | ((unsigned int)zipData[pos + 23] << 24);
		unsigned int uSize = (unsigned int)zipData[pos + 24]
		                   | ((unsigned int)zipData[pos + 25] << 8)
		                   | ((unsigned int)zipData[pos + 26] << 16)
		                   | ((unsigned int)zipData[pos + 27] << 24);
		int nameLen  = (int)zipData[pos + 28] | ((int)zipData[pos + 29] << 8);
		int extraLen = (int)zipData[pos + 30] | ((int)zipData[pos + 31] << 8);
		int commLen  = (int)zipData[pos + 32] | ((int)zipData[pos + 33] << 8);
		unsigned int relOff = (unsigned int)zipData[pos + 42]
		                    | ((unsigned int)zipData[pos + 43] << 8)
		                    | ((unsigned int)zipData[pos + 44] << 16)
		                    | ((unsigned int)zipData[pos + 45] << 24);

		if (nameLen > 0 && pos + 46 + nameLen <= cdEnd) {
			const char* name = (const char*)(zipData + pos + 46);
				if (nameLen >= 17 && strncmp(name, "word/document.xml", 17) == 0) {
				foundOff = (int)relOff;
				foundCSize = (int)cSize;
				foundUSize = (int)uSize;
				foundMethod = compMethod;
				break;
			}
		}

		pos += 46 + nameLen + extraLen + commLen;
	}

	if (foundOff < 0 || foundOff >= zipLen) { free(zipData); return 0; }

	/* 从本地文件头读取数据偏移 */
	int lhNameLen  = (int)zipData[foundOff + 26] | ((int)zipData[foundOff + 27] << 8);
	int lhExtraLen = (int)zipData[foundOff + 28] | ((int)zipData[foundOff + 29] << 8);
	int dataOff = foundOff + 30 + lhNameLen + lhExtraLen;

	const unsigned char* rawData = zipData + dataOff;
	int rawLen = (int)(zipLen - dataOff);
	if (foundCSize > 0 && foundCSize < rawLen) rawLen = foundCSize;

	/* 解压 */
	char* xmlText = NULL;
	int xmlLen = 0;

	if (foundMethod == 8 && foundUSize > 0 && rawLen > 0) {
		/* Deflate */
		xmlText = (char*)malloc((size_t)foundUSize + 1);
		if (xmlText) {
			int destLen = foundUSize;
			if (tinf_uncompress_raw(xmlText, &destLen, rawData, rawLen) == TINF_OK) {
				xmlLen = destLen;
				xmlText[destLen] = '\0';
			} else {
				free(xmlText); xmlText = NULL;
			}
		}
	} else if (foundMethod == 0 && rawLen > 0) {
		/* 未压缩 */
		xmlText = (char*)malloc((size_t)rawLen + 1);
		if (xmlText) {
			memcpy(xmlText, rawData, (size_t)rawLen);
			xmlLen = rawLen;
			xmlText[rawLen] = '\0';
		}
	}

	free(zipData);

	if (!xmlText || xmlLen == 0) {
		free(xmlText);
		return 0;
	}

	/* 将 XML 转为纯文本 (复用 HTML 解析器) */
	int outLen = parse_html_to_text(xmlText, xmlLen, out, maxOut);
	free(xmlText);
	return outLen;
}

/* ==================== 中英文混合分词器 ==================== */

/*
 * 中英文混合分词策略：
 *   - ASCII/拉丁字母：累积成英文单词（空白/标点分隔），转小写，过滤停用词
 *   - CJK 汉字 (U+4E00–U+9FFF, 扩展A/B)：每个单字作为一个词元 (unigram)，
 *     相邻 CJK 字符两两组合为二元组 (bigram)。例如"搜索引擎"产生：
 *     ["搜","搜索","索","索引","引","引擎","擎"]
 *   - CJK 标点：视为分隔符，触发 CJK 缓冲区的刷新
 *   - 混合文本：CJK↔ASCII 交界处自动切换，切换时刷新已有缓冲区
 *
 * 为什么用 Bigram：中文无明显词边界，单字粒度太细，Bigram 能捕获
 * 常见双字组合，在召回率和准确率之间取得平衡。
 */

/* 添加一个词元到文档的 words 数组（带停用词过滤） */
static int add_token(char** words, int* pCount, const char* token, int tokenLen)
{
	if (tokenLen <= 0 || *pCount >= MAX_DOC_WORDS) return 0;

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

	char  engBuf[MAX_WORD_LEN];
	int   engLen = 0;

	char  cjkChars[MAX_DOC_WORDS][4];
	int   cjkLens[MAX_DOC_WORDS];
	int   cjkCount = 0;

#define FLUSH_ENG() do { \
	if (engLen > 0) { \
		add_token(words, &wordCount, engBuf, engLen); \
		engLen = 0; \
	} \
} while(0)

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
				if (cjkCount > 0) FLUSH_CJK();
				if (engLen < MAX_WORD_LEN - 1) {
					engBuf[engLen++] = (char)tolower(ch);
				}
			}
		} else {
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
	if (g_stopwordCount == 0) {
		load_stopwords("stopwords.txt");
	}

	DIR* dir = opendir(folder_path);
	if (dir == NULL) {
		fprintf(stderr, "[text_io] Cannot open directory: %s\n", folder_path);
		*out_docs = NULL;
		*total_doc_num = 0;
		return -1;
	}

	/* 第一遍：统计支持的文件数 */
	struct dirent* entry;
	int fileCount = 0;
	while ((entry = readdir(dir)) != NULL) {
		if (is_supported_ext(entry->d_name)) fileCount++;
	}
	rewinddir(dir);

	if (fileCount == 0) {
		closedir(dir);
		*out_docs = NULL;
		*total_doc_num = 0;
		fprintf(stderr, "[text_io] No supported files found in %s\n", folder_path);
		return 0;
	}

	DocInfo* docs = (DocInfo*)malloc(sizeof(DocInfo) * (size_t)fileCount);
	int docIdx = 0;

	while ((entry = readdir(dir)) != NULL) {
		FileType ftype = get_file_type(entry->d_name);
		if (ftype == FILE_TYPE_UNKNOWN) continue;

		char fullPath[MAX_FILENAME_LEN + 512];
		snprintf(fullPath, sizeof(fullPath), "%s/%s", folder_path, entry->d_name);

		DocInfo* doc = &docs[docIdx];
		doc->id = docIdx;
		strncpy(doc->fileName, entry->d_name, MAX_FILENAME_LEN - 1);
		doc->fileName[MAX_FILENAME_LEN - 1] = '\0';
		doc->content[0] = '\0';

		int textLen = 0;

		switch (ftype) {
		case FILE_TYPE_TXT: {
			/* 纯文本文件 — 原有逻辑 */
			FILE* fp = fopen(fullPath, "r");
			if (!fp) {
				fprintf(stderr, "[text_io] Cannot open: %s\n", fullPath);
				break;
			}
			size_t readLen = fread(doc->content, 1, MAX_CONTENT_LEN - 1, fp);
			doc->content[readLen] = '\0';
			fclose(fp);
			textLen = (int)readLen;
			break;
		}
		case FILE_TYPE_HTML: {
			/* HTML 文件 */
			int rawLen = 0;
			char* raw = read_file_binary(fullPath, &rawLen);
			if (!raw) {
				fprintf(stderr, "[text_io] Cannot read HTML: %s\n", fullPath);
				break;
			}
			textLen = parse_html_to_text(raw, rawLen, doc->content, MAX_CONTENT_LEN - 1);
			free(raw);
			fprintf(stderr, "[text_io] Parsed HTML: %s (%d chars extracted)\n",
			        entry->d_name, textLen);
			break;
		}
		case FILE_TYPE_PDF: {
			/* PDF 文件 */
			int rawLen = 0;
			char* raw = read_file_binary(fullPath, &rawLen);
			if (!raw) {
				fprintf(stderr, "[text_io] Cannot read PDF: %s\n", fullPath);
				break;
			}
			textLen = pdf_extract_text(raw, rawLen, doc->content, MAX_CONTENT_LEN - 1);
			free(raw);
			fprintf(stderr, "[text_io] Parsed PDF: %s (%d chars extracted)\n",
			        entry->d_name, textLen);
			break;
		}
		case FILE_TYPE_DOCX: {
			/* DOCX 文件 */
			textLen = parse_docx_to_text(fullPath, doc->content, MAX_CONTENT_LEN - 1);
			fprintf(stderr, "[text_io] Parsed DOCX: %s (%d chars extracted)\n",
			        entry->d_name, textLen);
			break;
		}
		default:
			break;
		}

		/* ASCII 转小写 + 分词 */
		to_lower(doc->content);
		tokenize_doc(doc);

		fprintf(stderr, "[text_io] %s: %d tokens\n", entry->d_name, doc->wordNum);
		docIdx++;
	}
	closedir(dir);

	*out_docs = docs;
	*total_doc_num = docIdx;
	fprintf(stderr, "[text_io] Successfully loaded %d documents\n", docIdx);
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
