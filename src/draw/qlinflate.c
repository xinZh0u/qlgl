/**
 * @file    qlinflate.c
 * @brief   DEFLATE 解压。表结构与解码循环照 RFC 1951 + zlib 的 `puff` 来，
 *          刻意不用 2^15 的直查表（那是 64 KB 常驻，MCU 上不划算）。
 *
 * 为什么可以"输出缓冲就是窗口"：DEFLATE 的回溯距离最大 32768，而任何合法的
 * 回溯都指向**已经解出来的字节**（`dist <= len`），所以直接读 `out[len - dist]`
 * 就够，不需要单独的滑动窗口，也不需要把窗口搬来搬去。
 */
#include "qlinflate.h"

#include <string.h>

#include "qlgl/qlconfig.h"

#define ZMAXBITS   15
/* 最长可能是 286（literal/length）+ 30（distance），统一按 literal 表大小开 */
#define ZMAXSYMS   288
/* 输入暂存区：小到不心疼，大到把"每读一位就回调一次"摊薄 */
#define ZINBUF     256

typedef struct {
    int16_t count[ZMAXBITS + 1];   /* 每个码长有几个码 */
    int16_t symbol[ZMAXSYMS];      /* 按码长、再按符号序排好的符号表 */
} ZHuf;

typedef struct {
    const QLZSource *src;
    uint8_t  in[ZINBUF];
    size_t   in_pos, in_len;
    int      src_eof;

    uint32_t bitbuf;
    int      bitcnt;

    uint8_t *out;
    size_t   cap;
    size_t   len;
} ZBits;

/* ------------------------------ 位读取 ------------------------------ */

/* 把输入暂存区补满（至少 1 字节）。返回 0 = 流结束 */
static int zin_fill(ZBits *z)
{
    if (z->in_pos < z->in_len)
        return 1;
    if (z->src_eof)
        return 0;
    z->in_len = z->src->read(z->src->ctx, z->in, ZINBUF);
    z->in_pos = 0;
    if (z->in_len == 0) {
        z->src_eof = 1;
        return 0;
    }
    return 1;
}

/* 保证至少有 need 位（need <= 16）。返回 0 = 流不够长 */
static int zin_need(ZBits *z, int need)
{
    while (z->bitcnt < need) {
        if (!zin_fill(z))
            return 0;
        z->bitbuf |= (uint32_t)z->in[z->in_pos++] << z->bitcnt;
        z->bitcnt += 8;
    }
    return 1;
}

/* 读 need 位（低位先出）。need 必须 <= 16 */
static int zbits(ZBits *z, int need, uint32_t *out)
{
    if (need == 0) {
        *out = 0;
        return 1;
    }
    if (!zin_need(z, need))
        return 0;
    *out = z->bitbuf & ((1u << need) - 1u);
    z->bitbuf >>= need;
    z->bitcnt -= need;
    return 1;
}

/* 丢掉余下的位，对齐到字节边界（stored 块用） */
static void zalign(ZBits *z)
{
    const int drop = z->bitcnt & 7;
    z->bitbuf >>= drop;
    z->bitcnt -= drop;
}

static int zbyte(ZBits *z, uint8_t *out)
{
    uint32_t v;
    if (!zbits(z, 8, &v))
        return 0;
    *out = (uint8_t)v;
    return 1;
}

/* ------------------------------ 输出 ------------------------------ */

static int zput(ZBits *z, uint8_t v)
{
    if (z->len >= z->cap)
        return 0;
    z->out[z->len++] = v;
    return 1;
}

/* ------------------------------ Huffman ------------------------------ */

/*
 * 由码长表建规范 Huffman。返回 0 = 过度订阅（有些码长组合根本不可能出现）。
 * 全零（一个码都没有）返回 1：距离树允许为空（"这个块没有回溯"）。
 */
static int zh_build(ZHuf *h, const uint8_t *lens, int n)
{
    memset(h->count, 0, sizeof(h->count));
    for (int i = 0; i < n; i++)
        h->count[lens[i]]++;
    if (h->count[0] == n)
        return 1;

    int left = 1;
    for (int len = 1; len <= ZMAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return 0;                  /* 过度订阅 */
    }

    int16_t offs[ZMAXBITS + 1];
    offs[1] = 0;
    for (int len = 1; len < ZMAXBITS; len++)
        offs[len + 1] = (int16_t)(offs[len] + h->count[len]);
    for (int i = 0; i < n; i++)
        if (lens[i] != 0)
            h->symbol[offs[lens[i]]++] = (int16_t)i;
    return 1;
}

/* 逐位解一个符号（规范码的"先到先得"性质让这 10 行就够）。返回 -1 = 非法码 */
static int zh_decode(ZBits *z, const ZHuf *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= ZMAXBITS; len++) {
        uint32_t b;
        if (!zbits(z, 1, &b))
            return -1;
        code |= (int)b;
        const int count = h->count[len];
        if (code - first < count)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

/* ------------------------------ 块 ------------------------------ */

/* RFC 1951 §3.2.5 的长度/距离基值与额外位数 */
static const uint16_t k_len_base[29] = {
    3,  4,  5,  6,  7,  8,  9,  10, 11, 13, 15, 17, 19, 23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
};
static const uint8_t k_len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
};
static const uint16_t k_dist_base[30] = {
    1,    2,    3,    4,    5,    7,     9,     13,    17,    25,
    33,   49,   65,   97,   129,  193,   257,   385,   513,   769,
    1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577,
};
static const uint8_t k_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
};
/* 码长码自身被"打乱"的顺序（RFC 1951 §3.2.7） */
static const uint8_t k_clen_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15,
};

/* 固定 Huffman（BTYPE=1） */
static int zh_fixed(ZHuf *lit, ZHuf *dist)
{
    uint8_t l[288];
    for (int i = 0; i < 144; i++)
        l[i] = 8;
    for (int i = 144; i < 256; i++)
        l[i] = 9;
    for (int i = 256; i < 280; i++)
        l[i] = 7;
    for (int i = 280; i < 288; i++)
        l[i] = 8;
    if (!zh_build(lit, l, 288))
        return 0;

    uint8_t d[30];
    memset(d, 5, sizeof(d));
    return zh_build(dist, d, 30);
}

/* 动态 Huffman（BTYPE=2） */
static int zh_dynamic(ZBits *z, ZHuf *lit, ZHuf *dist)
{
    uint32_t v;
    if (!zbits(z, 5, &v))
        return 0;
    const int hlit = (int)v + 257;
    if (!zbits(z, 5, &v))
        return 0;
    const int hdist = (int)v + 1;
    if (!zbits(z, 4, &v))
        return 0;
    const int hclen = (int)v + 4;
    if (hlit > 286 || hdist > 30)
        return 0;

    uint8_t cl[19];
    memset(cl, 0, sizeof(cl));
    for (int i = 0; i < hclen; i++) {
        if (!zbits(z, 3, &v))
            return 0;
        cl[k_clen_order[i]] = (uint8_t)v;
    }
    ZHuf clh;
    if (!zh_build(&clh, cl, 19))
        return 0;

    /* 先用"码长码"解出两棵树的码长，重复码 16/17/18 负责压缩连续 0 与重复 */
    uint8_t lens[286 + 30];
    memset(lens, 0, sizeof(lens));
    const int total = hlit + hdist;
    int n = 0;
    while (n < total) {
        const int sym = zh_decode(z, &clh);
        if (sym < 0)
            return 0;
        if (sym < 16) {
            lens[n++] = (uint8_t)sym;
            continue;
        }
        int rep = 0, val = 0;
        if (sym == 16) {
            if (n == 0)
                return 0;              /* 没有前一个码长可重复 */
            val = lens[n - 1];
            if (!zbits(z, 2, &v))
                return 0;
            rep = 3 + (int)v;
        } else if (sym == 17) {
            if (!zbits(z, 3, &v))
                return 0;
            rep = 3 + (int)v;
        } else {
            if (!zbits(z, 7, &v))
                return 0;
            rep = 11 + (int)v;
        }
        if (n + rep > total)
            return 0;
        while (rep-- > 0)
            lens[n++] = (uint8_t)val;
    }

    if (lens[256] == 0)
        return 0;                      /* 没有 end-of-block，这个流没法结束 */
    if (!zh_build(lit, lens, hlit))
        return 0;
    if (!zh_build(dist, lens + hlit, hdist))
        return 0;
    return 1;
}

/*
 * 解一个压缩块的数据部分。返回 1 = 读到 end-of-block；0 = 出错。
 * `why` 用来区分"数据坏了"和"输出装不下"（后者不是数据的问题）。
 */
static int zblock_data(ZBits *z, const ZHuf *lit, const ZHuf *dist, int *why)
{
    for (;;) {
        const int sym = zh_decode(z, lit);
        if (sym < 0) {
            *why = QLZ_ERR_DATA;
            return 0;
        }
        if (sym < 256) {
            if (!zput(z, (uint8_t)sym)) {
                *why = QLZ_ERR_SPACE;
                return 0;
            }
            continue;
        }
        if (sym == 256)
            return 1;                  /* 块结束 */

        const int li = sym - 257;
        if (li >= 29) {
            *why = QLZ_ERR_DATA;
            return 0;
        }
        uint32_t extra = 0;
        if (!zbits(z, k_len_extra[li], &extra)) {
            *why = QLZ_ERR_INCOMP;
            return 0;
        }
        size_t ncopy = (size_t)k_len_base[li] + extra;

        const int dsym = zh_decode(z, dist);
        if (dsym < 0 || dsym >= 30) {
            *why = QLZ_ERR_DATA;
            return 0;
        }
        if (!zbits(z, k_dist_extra[dsym], &extra)) {
            *why = QLZ_ERR_INCOMP;
            return 0;
        }
        const size_t back = (size_t)k_dist_base[dsym] + extra;
        if (back > z->len) {           /* 回溯越界：指向还没解出来的字节 */
            *why = QLZ_ERR_DATA;
            return 0;
        }
        if (ncopy > z->cap - z->len) {
            *why = QLZ_ERR_SPACE;
            return 0;
        }
        /* 逐字节复制：源可能和目标是同一块内存且重叠，memcpy 在这里是错的 */
        for (size_t i = 0; i < ncopy; i++) {
            z->out[z->len] = z->out[z->len - back];
            z->len++;
        }
    }
}

static int zstored(ZBits *z, int *why)
{
    zalign(z);
    uint8_t b[4];
    for (int i = 0; i < 4; i++) {
        if (!zbyte(z, &b[i])) {
            *why = QLZ_ERR_INCOMP;
            return 0;
        }
    }
    const uint32_t len = (uint32_t)b[0] | ((uint32_t)b[1] << 8);
    const uint32_t nlen = (uint32_t)b[2] | ((uint32_t)b[3] << 8);
    if ((len ^ 0xFFFFu) != nlen) {
        *why = QLZ_ERR_DATA;
        return 0;
    }
    if ((size_t)len > z->cap - z->len) {
        *why = QLZ_ERR_SPACE;
        return 0;
    }
    for (uint32_t i = 0; i < len; i++) {
        uint8_t v;
        if (!zbyte(z, &v)) {
            *why = QLZ_ERR_INCOMP;
            return 0;
        }
        z->out[z->len++] = v;
    }
    return 1;
}

int ql_inflate(const QLZSource *src, uint8_t *dst, size_t dst_cap, size_t *out_len)
{
    if (out_len != NULL)
        *out_len = 0;
    if (src == NULL || src->read == NULL || (dst == NULL && dst_cap > 0))
        return QLZ_ERR_DATA;

    ZBits z;
    memset(&z, 0, sizeof(z));
    z.src = src;
    z.out = dst;
    z.cap = dst_cap;

    ZHuf lit, dist;
    int why = QLZ_ERR_DATA;
    int last = 0;
    while (!last) {
        uint32_t v;
        if (!zbits(&z, 1, &v)) {
            why = QLZ_ERR_INCOMP;
            goto fail;
        }
        last = (int)v;
        if (!zbits(&z, 2, &v)) {
            why = QLZ_ERR_INCOMP;
            goto fail;
        }
        switch ((int)v) {
        case 0:
            if (!zstored(&z, &why))
                goto fail;
            break;
        case 1:
            if (!zh_fixed(&lit, &dist)) {
                why = QLZ_ERR_DATA;
                goto fail;
            }
            if (!zblock_data(&z, &lit, &dist, &why))
                goto fail;
            break;
        case 2:
            if (!zh_dynamic(&z, &lit, &dist)) {
                why = QLZ_ERR_INCOMP;  /* 码表本身读不完或非法 */
                goto fail;
            }
            if (!zblock_data(&z, &lit, &dist, &why))
                goto fail;
            break;
        default:
            why = QLZ_ERR_DATA;        /* BTYPE=3 保留 */
            goto fail;
        }
    }

    if (out_len != NULL)
        *out_len = z.len;
    return QLZ_OK;

fail:
    if (out_len != NULL)
        *out_len = z.len;
    return why;
}
