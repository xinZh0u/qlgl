/**
 * @file    test_png.c
 * @brief   PNG 解码：容器 / 五种颜色类型 × 五种位深 / 五种行过滤 / 错误路径。
 *
 * 输入不是提交的二进制文件，而是**测试里现场构造**的 PNG：只用 stored（未压缩）
 * deflate 块，所以"喂进去的像素是什么"完全可控，坏例也能精确构造到某一处。
 * 压缩数据（动态 Huffman）的覆盖面另由 `test/assets/png/` 里 PIL 生成的文件
 * 加 `qltext` 对拍兜住。
 *
 * 每个组合断言的是**具体像素值**，不是"没崩就行"。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ql_test.h"

/* ------------------------------ 极简 PNG 构造器 ------------------------------ */

typedef struct {
    uint8_t *p;
    size_t   n, cap;
} Buf;

static void bneed(Buf *b, size_t extra)
{
    if (b->n + extra <= b->cap)
        return;
    size_t cap = (b->cap > 0) ? b->cap : 256;
    while (cap < b->n + extra)
        cap *= 2;
    uint8_t *np = (uint8_t *)realloc(b->p, cap);
    if (np == NULL)
        abort();
    b->p = np;
    b->cap = cap;
}

static void bput(Buf *b, const void *d, size_t n)
{
    bneed(b, n);
    memcpy(b->p + b->n, d, n);
    b->n += n;
}

static void bbyte(Buf *b, uint8_t v)
{
    bput(b, &v, 1);
}

static void bbe32(Buf *b, uint32_t v)
{
    uint8_t t[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8),
                     (uint8_t)v };
    bput(b, t, 4);
}

static uint32_t crc32_of(const uint8_t *d, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1u)));
    }
    return c ^ 0xFFFFFFFFu;
}

static void bchunk(Buf *b, const uint8_t *type, const uint8_t *data, uint32_t n)
{
    bbe32(b, n);
    const size_t tpos = b->n;
    bput(b, type, 4);
    if (n > 0)
        bput(b, data, n);
    const uint32_t crc = crc32_of(b->p + tpos, (size_t)n + 4u);
    bbe32(b, crc);
}

static uint32_t adler32_of(const uint8_t *p, size_t n)
{
    uint32_t a = 1, bb = 0;
    while (n > 0) {
        size_t chunk = (n < 5552u) ? n : 5552u;
        n -= chunk;
        while (chunk-- > 0) {
            a += *p++;
            bb += a;
        }
        a %= 65521u;
        bb %= 65521u;
    }
    return (bb << 16) | a;
}

/* 用 stored 块包成 zlib 数据流（PNG 的 IDAT 就是这个） */
static void bzlib_stored(Buf *b, const uint8_t *raw, size_t n)
{
    bbyte(b, 0x78);
    bbyte(b, 0x01);                    /* (0x7801 % 31) == 0，FLEVEL=0 */
    size_t off = 0;
    do {
        size_t take = n - off;
        if (take > 65535u)
            take = 65535u;
        const int last = (off + take >= n);
        bbyte(b, (uint8_t)(last ? 1 : 0));   /* BFINAL, BTYPE=00 */
        bbyte(b, (uint8_t)(take & 0xFFu));
        bbyte(b, (uint8_t)(take >> 8));
        bbyte(b, (uint8_t)(~take & 0xFFu));
        bbyte(b, (uint8_t)((~take >> 8) & 0xFFu));
        if (take > 0)
            bput(b, raw + off, take);
        off += take;
    } while (off < n);
    bbe32(b, adler32_of(raw, n));
}

/* 正向行过滤（编码侧；与解码器的反过滤互为逆运算） */
static void png_filter_row(int ft, uint8_t *out, const uint8_t *cur, const uint8_t *prev,
                           size_t n, size_t bpp)
{
    for (size_t i = 0; i < n; i++) {
        const int a = (i >= bpp) ? cur[i - bpp] : 0;
        const int bb = (prev != NULL) ? prev[i] : 0;
        const int c = (prev != NULL && i >= bpp) ? prev[i - bpp] : 0;
        int pred = 0;
        switch (ft) {
        case 0: pred = 0; break;
        case 1: pred = a; break;
        case 2: pred = bb; break;
        case 3: pred = (a + bb) >> 1; break;
        default: {
            const int p = a + bb - c;
            const int pa = (p > a) ? p - a : a - p;
            const int pb = (p > bb) ? p - bb : bb - p;
            const int pc = (p > c) ? p - c : c - p;
            pred = (pa <= pb && pa <= pc) ? a : ((pb <= pc) ? bb : c);
            break;
        }
        }
        out[i] = (uint8_t)(cur[i] - pred);
    }
}

/* 一个待构造的 PNG 的全部输入 */
typedef struct {
    int      w, h;
    int      depth, ctype;
    int      interlace;      /* 0 = 无；非 0 用来测"明确不支持" */
    int      comp, filtm;    /* 覆盖成非 0 用来测"明确不支持" */
    const uint8_t *plte;     /* 3*n，调色板 */
    int      plte_n;
    const uint8_t *trns;     /* tRNS 原始字节（原样写进去） */
    int      trns_n;
    const int *filters;      /* 每行的过滤类型；NULL = 全 0 */
    int      idat_parts;     /* IDAT 拆成几段（1 = 不拆） */
} PngSpec;

/*
 * "未过滤的行字节"总长度。构造器没法从 (w,h,ctype,depth) 反推调用方到底准备了
 * 几行（有些坏例故意让 IHDR 与实际数据不一致），所以由调用方显式给。
 */
static size_t g_png_raw_len;

/* 构造整份 PNG 文件（含签名） */
static void build_png(Buf *out, const PngSpec *sp, const uint8_t *raw)
{
    static const uint8_t k_sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    bput(out, k_sig, 8);

    uint8_t ihdr[13];
    ihdr[0] = (uint8_t)(sp->w >> 24);
    ihdr[1] = (uint8_t)(sp->w >> 16);
    ihdr[2] = (uint8_t)(sp->w >> 8);
    ihdr[3] = (uint8_t)sp->w;
    ihdr[4] = (uint8_t)(sp->h >> 24);
    ihdr[5] = (uint8_t)(sp->h >> 16);
    ihdr[6] = (uint8_t)(sp->h >> 8);
    ihdr[7] = (uint8_t)sp->h;
    ihdr[8] = (uint8_t)sp->depth;
    ihdr[9] = (uint8_t)sp->ctype;
    ihdr[10] = (uint8_t)sp->comp;
    ihdr[11] = (uint8_t)sp->filtm;
    ihdr[12] = (uint8_t)sp->interlace;
    bchunk(out, (const uint8_t *)"IHDR", ihdr, 13u);

    if (sp->plte != NULL && sp->plte_n > 0)
        bchunk(out, (const uint8_t *)"PLTE", sp->plte, (uint32_t)sp->plte_n * 3u);
    if (sp->trns != NULL && sp->trns_n > 0)
        bchunk(out, (const uint8_t *)"tRNS", sp->trns, (uint32_t)sp->trns_n);

    Buf z;
    memset(&z, 0, sizeof(z));
    bzlib_stored(&z, raw, g_png_raw_len);

    const int parts = (sp->idat_parts > 0) ? sp->idat_parts : 1;
    size_t per = (z.n + (size_t)parts - 1u) / (size_t)parts;
    if (per == 0)
        per = 1;
    for (int i = 0; i < parts; i++) {
        size_t off = per * (size_t)i;
        if (off >= z.n)
            break;
        size_t take = z.n - off;
        if (take > per)
            take = per;
        bchunk(out, (const uint8_t *)"IDAT", z.p + off, (uint32_t)take);
    }
    free(z.p);

    bchunk(out, (const uint8_t *)"IEND", NULL, 0u);
}

/* ------------------------------ 期望值 ------------------------------ */

/* 采样值 → 8 位（与解码器同一套量化规则；PIL 对拍负责兜底） */
static int sample_to8(int depth, uint32_t s)
{
    switch (depth) {
    case 1: return s ? 255 : 0;
    case 2: return (int)(s * 85u);
    case 4: return (int)(s * 17u);
    case 8: return (int)s;
    default: return (int)(s >> 8);
    }
}

/* 第 (x, 通道 j) 个采样值；pattern 固定，两边都能算 */
static uint32_t sample_of(int depth, int x, int j)
{
    const int k = (x * 3 + j * 5) & 0xFF;
    switch (depth) {
    case 1: return (uint32_t)(k & 1);
    case 2: return (uint32_t)(k & 3);
    case 4: return (uint32_t)(k & 15);
    case 8: return (uint32_t)k;
    default: return (uint32_t)(k * 257);   /* 高低字节相同，>>8 无损 */
    }
}

/* 按 (ctype, depth) 生成"未过滤"的原始行字节 */
static void make_rows(int ctype, int depth, int w, int h, uint8_t *rows,
                      size_t row_bytes, size_t bpp)
{
    for (int y = 0; y < h; y++) {
        uint8_t *row = rows + (size_t)y * (row_bytes + 1u);
        memset(row, 0, row_bytes + 1u);
        for (int x = 0; x < w; x++) {
            int chans = 0;
            switch (ctype) {
            case 0: chans = 1; break;
            case 2: chans = 3; break;
            case 3: chans = 1; break;
            case 4: chans = 2; break;
            default: chans = 4; break;
            }
            for (int j = 0; j < chans; j++) {
                const uint32_t s = sample_of(depth, x + y, j);
                const size_t idx = (size_t)x * (size_t)chans + (size_t)j;
                if (depth == 8) {
                    row[1 + idx] = (uint8_t)s;
                } else if (depth == 16) {
                    row[1 + idx * 2u] = (uint8_t)(s >> 8);
                    row[1 + idx * 2u + 1u] = (uint8_t)s;
                } else {
                    const size_t bit = idx * (size_t)depth;
                    row[1 + (bit >> 3)] |= (uint8_t)(s << (8u - (uint32_t)depth -
                                                          (bit & 7u)));
                }
            }
        }
        (void)bpp;
    }
}

/* 解码一张图并按 RGBA8888 取出第 (x,y) 个像素 */
static int decode_rgba(const uint8_t *data, size_t len, QLImage *out)
{
    QLImageDecodeReq req;
    memset(&req, 0, sizeof(req));
    req.data = data;
    req.len = len;
    req.src_format = QL_IMAGE_SRC_AUTO;
    return ql_image_decode(&req, out);
}

static void px_rgba(const QLImage *img, int x, int y, uint8_t *rgba)
{
    const uint8_t *p = img->px + (size_t)y * img->stride + (size_t)x * 4u;
    memcpy(rgba, p, 4);
}

static void free_img(QLImage *img)
{
    if (img->owned && img->px != NULL)
        ql_mem_free((void *)img->px);
}

QL_TEST_BEGIN("test_png")

#if !(QL_FEAT_IMG_PNG && QL_FEAT_IMG_INFLATE)
    /*
     * 这个构建把 PNG 裁了（例如 QLGL_IMG_PRESET=raw-only）：容器、位深、
     * 过滤这些都没得测，但"识别出格式却明确说解不了"这条必须守住。
     */
    {
        static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
        QLImageDecodeReq req;
        memset(&req, 0, sizeof(req));
        req.data = sig;
        req.len = sizeof(sig);
        req.src_format = QL_IMAGE_SRC_AUTO;
        QLImage img;
        QL_CHECK_EQ(ql_image_decode(&req, &img), QL_ERR_UNSUPPORTED);
        req.src_format = QL_IMAGE_SRC_PNG;      /* 显式指定也一样 */
        QL_CHECK_EQ(ql_image_decode(&req, &img), QL_ERR_UNSUPPORTED);
    }
#else
    /* ---------- 1. 五种颜色类型 × 各自合法的位深 ---------- */
    {
        static const struct {
            int ctype, depth;
        } k_combos[] = {
            { 0, 1 },  { 0, 2 },  { 0, 4 },  { 0, 8 },  { 0, 16 },
            { 2, 8 },  { 2, 16 },
            { 3, 1 },  { 3, 2 },  { 3, 4 },  { 3, 8 },
            { 4, 8 },  { 4, 16 },
            { 6, 8 },  { 6, 16 },
        };
        const int W = 5, H = 3;
        for (size_t c = 0; c < sizeof(k_combos) / sizeof(k_combos[0]); c++) {
            const int ctype = k_combos[c].ctype;
            const int depth = k_combos[c].depth;
            int chans = (ctype == 0 || ctype == 3) ? 1
                        : (ctype == 2) ? 3
                        : (ctype == 4) ? 2
                                       : 4;
            const size_t row_bytes =
                ((size_t)W * (size_t)chans * (size_t)depth + 7u) / 8u;
            const size_t bpp = ((size_t)chans * (size_t)depth + 7u) / 8u;

            uint8_t *rows = (uint8_t *)calloc((size_t)H, row_bytes + 1u);
            QL_CHECK(rows != NULL);
            make_rows(ctype, depth, W, H, rows, row_bytes, bpp);

            /* 调色板：把索引 i 映射成 (i*10, i*20, i*30) */
            uint8_t plte[256 * 3];
            int plte_n = 0;
            if (ctype == 3) {
                plte_n = 1 << depth;
                for (int i = 0; i < plte_n; i++) {
                    plte[i * 3] = (uint8_t)((i * 10) & 0xFF);
                    plte[i * 3 + 1] = (uint8_t)((i * 20) & 0xFF);
                    plte[i * 3 + 2] = (uint8_t)((i * 30) & 0xFF);
                }
            }

            PngSpec sp;
            memset(&sp, 0, sizeof(sp));
            sp.w = W;
            sp.h = H;
            sp.depth = depth;
            sp.ctype = ctype;
            sp.plte = (ctype == 3) ? plte : NULL;
            sp.plte_n = plte_n;
            sp.idat_parts = 1;

            g_png_raw_len = (size_t)H * (row_bytes + 1u);
            Buf file;
            memset(&file, 0, sizeof(file));
            build_png(&file, &sp, rows);
            free(rows);

            QLImage img;
            const int rc = decode_rgba(file.p, file.n, &img);
            QL_CHECK_EQ(rc, QL_OK);
            if (rc == QL_OK) {
                QL_CHECK_EQ((int)img.w, W);
                QL_CHECK_EQ((int)img.h, H);
                QL_CHECK_EQ((int)img.format, (int)QL_IMAGE_RGBA8888);
                for (int y = 0; y < H; y++) {
                    for (int x = 0; x < W; x++) {
                        uint8_t got[4];
                        px_rgba(&img, x, y, got);
                        int er, eg, eb, ea;
                        if (ctype == 3) {
                            const uint32_t idx = sample_of(depth, x + y, 0);
                            er = (int)((idx * 10u) & 0xFFu);
                            eg = (int)((idx * 20u) & 0xFFu);
                            eb = (int)((idx * 30u) & 0xFFu);
                            ea = 255;
                        } else if (ctype == 0) {
                            er = eg = eb = sample_to8(depth, sample_of(depth, x + y, 0));
                            ea = 255;
                        } else if (ctype == 2) {
                            er = sample_to8(depth, sample_of(depth, x + y, 0));
                            eg = sample_to8(depth, sample_of(depth, x + y, 1));
                            eb = sample_to8(depth, sample_of(depth, x + y, 2));
                            ea = 255;
                        } else if (ctype == 4) {
                            er = eg = eb = sample_to8(depth, sample_of(depth, x + y, 0));
                            ea = sample_to8(depth, sample_of(depth, x + y, 1));
                        } else {
                            er = sample_to8(depth, sample_of(depth, x + y, 0));
                            eg = sample_to8(depth, sample_of(depth, x + y, 1));
                            eb = sample_to8(depth, sample_of(depth, x + y, 2));
                            ea = sample_to8(depth, sample_of(depth, x + y, 3));
                        }
                        QL_CHECK_EQ((int)got[0], er);
                        QL_CHECK_EQ((int)got[1], eg);
                        QL_CHECK_EQ((int)got[2], eb);
                        QL_CHECK_EQ((int)got[3], ea);
                    }
                }
                free_img(&img);
            }
            free(file.p);
        }
    }

    /* ---------- 2. 五种行过滤（RGB8 与 RGBA16，bpp 不同） ---------- */
    {
        static const struct {
            int ctype, depth;
        } k_cases[] = { { 2, 8 }, { 6, 16 }, { 0, 4 }, { 4, 8 } };
        const int W = 6, H = 5;
        for (size_t c = 0; c < sizeof(k_cases) / sizeof(k_cases[0]); c++) {
            const int ctype = k_cases[c].ctype;
            const int depth = k_cases[c].depth;
            int chans = (ctype == 0) ? 1 : (ctype == 2) ? 3 : (ctype == 4) ? 2 : 4;
            const size_t row_bytes =
                ((size_t)W * (size_t)chans * (size_t)depth + 7u) / 8u;
            const size_t bpp = ((size_t)chans * (size_t)depth + 7u) / 8u;

            uint8_t *rows = (uint8_t *)calloc((size_t)H, row_bytes + 1u);
            QL_CHECK(rows != NULL);
            make_rows(ctype, depth, W, H, rows, row_bytes, bpp);

            /* 每行换一种过滤（0..4 循环），把"未过滤的行"就地过滤成 PNG 行 */
            int filters[8];
            for (int y = 0; y < H; y++)
                filters[y] = y % 5;
            for (int y = H - 1; y >= 0; y--) {
                uint8_t *cur = rows + (size_t)y * (row_bytes + 1u) + 1u;
                const uint8_t *prev =
                    (y > 0) ? (rows + (size_t)(y - 1) * (row_bytes + 1u) + 1u) : NULL;
                uint8_t *tmp = (uint8_t *)malloc(row_bytes);
                QL_CHECK(tmp != NULL);
                memcpy(tmp, cur, row_bytes);
                png_filter_row(filters[y], cur, tmp, prev, row_bytes, bpp);
                rows[(size_t)y * (row_bytes + 1u)] = (uint8_t)filters[y];
                free(tmp);
            }

            PngSpec sp;
            memset(&sp, 0, sizeof(sp));
            sp.w = W;
            sp.h = H;
            sp.depth = depth;
            sp.ctype = ctype;
            sp.idat_parts = 1;

            g_png_raw_len = (size_t)H * (row_bytes + 1u);
            Buf file;
            memset(&file, 0, sizeof(file));
            build_png(&file, &sp, rows);
            free(rows);

            QLImage img;
            const int rc = decode_rgba(file.p, file.n, &img);
            QL_CHECK_EQ(rc, QL_OK);
            if (rc == QL_OK) {
                for (int y = 0; y < H; y++) {
                    for (int x = 0; x < W; x++) {
                        uint8_t got[4];
                        px_rgba(&img, x, y, got);
                        if (ctype == 2) {
                            QL_CHECK_EQ((int)got[0], sample_to8(depth, sample_of(depth, x + y, 0)));
                            QL_CHECK_EQ((int)got[1], sample_to8(depth, sample_of(depth, x + y, 1)));
                            QL_CHECK_EQ((int)got[2], sample_to8(depth, sample_of(depth, x + y, 2)));
                        } else if (ctype == 0) {
                            QL_CHECK_EQ((int)got[1], sample_to8(depth, sample_of(depth, x + y, 0)));
                        } else if (ctype == 4) {
                            QL_CHECK_EQ((int)got[0], sample_to8(depth, sample_of(depth, x + y, 0)));
                            QL_CHECK_EQ((int)got[3], sample_to8(depth, sample_of(depth, x + y, 1)));
                        } else {
                            QL_CHECK_EQ((int)got[3], sample_to8(depth, sample_of(depth, x + y, 3)));
                        }
                    }
                }
                free_img(&img);
            }
            free(file.p);
        }
    }

    /* ---------- 3. IDAT 拆成多段（中间夹 chunk 头 / CRC） ---------- */
    {
        const int W = 4, H = 4;
        const size_t row_bytes = (size_t)W * 3u;
        uint8_t *rows = (uint8_t *)calloc((size_t)H, row_bytes + 1u);
        QL_CHECK(rows != NULL);
        make_rows(2, 8, W, H, rows, row_bytes, 3);

        PngSpec sp;
        memset(&sp, 0, sizeof(sp));
        sp.w = W;
        sp.h = H;
        sp.depth = 8;
        sp.ctype = 2;
        sp.idat_parts = 4;

        g_png_raw_len = (size_t)H * (row_bytes + 1u);
        Buf file;
        memset(&file, 0, sizeof(file));
        build_png(&file, &sp, rows);
        free(rows);

        QLImage img;
        QL_CHECK_EQ(decode_rgba(file.p, file.n, &img), QL_OK);
        if (img.px != NULL) {
            uint8_t got[4];
            px_rgba(&img, 2, 3, got);
            QL_CHECK_EQ((int)got[1], sample_to8(8, sample_of(8, 2 + 3, 1)));
            free_img(&img);
        }
        free(file.p);
    }

    /* ---------- 4. tRNS：调色板 / 灰度 / 真彩的透明色 ---------- */
    {
        /* 4.1 调色板：索引 1 透明 */
        const int W = 2, H = 1;
        uint8_t plte[2 * 3] = { 10, 20, 30, 40, 50, 60 };
        uint8_t trns[2] = { 255, 0 };
        uint8_t rows[3] = { 0, 0, 1 }; /* filter 0，两个索引 0 和 1 */

        PngSpec sp;
        memset(&sp, 0, sizeof(sp));
        sp.w = W;
        sp.h = H;
        sp.depth = 8;
        sp.ctype = 3;
        sp.plte = plte;
        sp.plte_n = 2;
        sp.trns = trns;
        sp.trns_n = 2;
        sp.idat_parts = 1;

        g_png_raw_len = sizeof(rows);
        Buf file;
        memset(&file, 0, sizeof(file));
        build_png(&file, &sp, rows);

        QLImage img;
        QL_CHECK_EQ(decode_rgba(file.p, file.n, &img), QL_OK);
        if (img.px != NULL) {
            uint8_t a[4], b[4];
            px_rgba(&img, 0, 0, a);
            px_rgba(&img, 1, 0, b);
            QL_CHECK_EQ((int)a[0], 10);
            QL_CHECK_EQ((int)a[3], 255);
            QL_CHECK_EQ((int)b[0], 40);
            QL_CHECK_EQ((int)b[3], 0);
            free_img(&img);
        }
        free(file.p);

        /* 4.2 灰度 8 位：值 7 透明 */
        uint8_t t16[2] = { 0, 7 };     /* tRNS 大端 16 位 */
        uint8_t grows[2] = { 0, 7 };
        memset(&sp, 0, sizeof(sp));
        sp.w = 1;
        sp.h = 1;
        sp.depth = 8;
        sp.ctype = 0;
        sp.trns = t16;
        sp.trns_n = 2;
        g_png_raw_len = sizeof(grows);
        memset(&file, 0, sizeof(file));
        build_png(&file, &sp, grows);
        QL_CHECK_EQ(decode_rgba(file.p, file.n, &img), QL_OK);
        if (img.px != NULL) {
            uint8_t a[4];
            px_rgba(&img, 0, 0, a);
            QL_CHECK_EQ((int)a[0], 7);
            QL_CHECK_EQ((int)a[3], 0);
            free_img(&img);
        }
        free(file.p);
    }

    /* ---------- 5. 明确失败：结构与语义错误 ---------- */
    {
        const int W = 2, H = 2;
        const size_t row_bytes = (size_t)W * 3u;
        uint8_t rows[2 * (2 * 3 + 1)];
        memset(rows, 0, sizeof(rows));
        make_rows(2, 8, W, H, rows, row_bytes, 3);

        PngSpec base;
        memset(&base, 0, sizeof(base));
        base.w = W;
        base.h = H;
        base.depth = 8;
        base.ctype = 2;
        base.idat_parts = 1;
        g_png_raw_len = sizeof(rows);

        /* 先确认基准是好的，后面每一条只改一个字段 */
        Buf file;
        memset(&file, 0, sizeof(file));
        build_png(&file, &base, rows);
        QLImage img;
        QL_CHECK_EQ(decode_rgba(file.p, file.n, &img), QL_OK);
        if (img.px != NULL)
            free_img(&img);

        /* 5.1 签名不对 */
        QL_CHECK_EQ(decode_rgba(file.p + 1, file.n - 1u, &img), QL_ERR_UNSUPPORTED);

        /* 5.2 隔行扫描 */
        {
            PngSpec sp = base;
            sp.interlace = 1;
            Buf f;
            memset(&f, 0, sizeof(f));
            build_png(&f, &sp, rows);
            QL_CHECK_EQ(decode_rgba(f.p, f.n, &img), QL_ERR_UNSUPPORTED);
            free(f.p);
        }
        /* 5.3 压缩方法 / 过滤方法非 0 */
        {
            PngSpec sp = base;
            sp.comp = 1;
            Buf f;
            memset(&f, 0, sizeof(f));
            build_png(&f, &sp, rows);
            QL_CHECK_EQ(decode_rgba(f.p, f.n, &img), QL_ERR_UNSUPPORTED);
            free(f.p);

            sp = base;
            sp.filtm = 1;
            memset(&f, 0, sizeof(f));
            build_png(&f, &sp, rows);
            QL_CHECK_EQ(decode_rgba(f.p, f.n, &img), QL_ERR_UNSUPPORTED);
            free(f.p);
        }
        /* 5.4 位深与颜色类型不搭（真彩只能是 8/16，调色板不能 16） */
        {
            PngSpec sp = base;
            sp.depth = 4;
            Buf f;
            memset(&f, 0, sizeof(f));
            build_png(&f, &sp, rows);
            QL_CHECK_EQ(decode_rgba(f.p, f.n, &img), QL_ERR_INVAL);
            free(f.p);

            sp = base;
            sp.ctype = 3;
            sp.depth = 16;
            memset(&f, 0, sizeof(f));
            build_png(&f, &sp, rows);
            QL_CHECK_EQ(decode_rgba(f.p, f.n, &img), QL_ERR_INVAL);
            free(f.p);

            sp = base;
            sp.ctype = 7;               /* 未定义的颜色类型 */
            memset(&f, 0, sizeof(f));
            build_png(&f, &sp, rows);
            QL_CHECK_EQ(decode_rgba(f.p, f.n, &img), QL_ERR_INVAL);
            free(f.p);
        }
        /* 5.5 调色板图缺 PLTE */
        {
            PngSpec sp = base;
            sp.ctype = 3;
            sp.depth = 8;
            Buf f;
            memset(&f, 0, sizeof(f));
            build_png(&f, &sp, rows);
            QL_CHECK_EQ(decode_rgba(f.p, f.n, &img), QL_ERR_INVAL);
            free(f.p);
        }
        /* 5.6 调色板索引越界（索引 5，但只有 2 个颜色） */
        {
            uint8_t plte[2 * 3] = { 1, 2, 3, 4, 5, 6 };
            uint8_t bad[2] = { 0, 5 };
            PngSpec sp = base;
            sp.ctype = 3;
            sp.w = 1;
            sp.h = 1;
            sp.plte = plte;
            sp.plte_n = 2;
            g_png_raw_len = sizeof(bad);
            Buf f;
            memset(&f, 0, sizeof(f));
            build_png(&f, &sp, bad);
            QL_CHECK_EQ(decode_rgba(f.p, f.n, &img), QL_ERR_INVAL);
            free(f.p);
            g_png_raw_len = sizeof(rows);
        }
        /* 5.7 尺寸为 0 / 超上限 */
        {
            PngSpec sp = base;
            sp.w = 0;
            Buf f;
            memset(&f, 0, sizeof(f));
            build_png(&f, &sp, rows);
            QL_CHECK_EQ(decode_rgba(f.p, f.n, &img), QL_ERR_INVAL);
            free(f.p);

            sp = base;
            sp.w = 65535;
            sp.h = 65535;               /* 先除后乘必须挡住 */
            memset(&f, 0, sizeof(f));
            build_png(&f, &sp, rows);
            QL_CHECK_EQ(decode_rgba(f.p, f.n, &img), QL_ERR_INVAL);
            free(f.p);
        }
        /* 5.8 坏过滤类型（5）：两行的过滤字节都改成 5 */
        {
            uint8_t bad[sizeof(rows)];
            memcpy(bad, rows, sizeof(bad));
            bad[0] = 5;
            bad[row_bytes + 1u] = 5;
            Buf f;
            memset(&f, 0, sizeof(f));
            build_png(&f, &base, bad);
            QL_CHECK_EQ(decode_rgba(f.p, f.n, &img), QL_ERR_INVAL);
            free(f.p);
        }
        /* 5.9 截断：砍掉 IEND 之后的一切（zlib 流/Alder 都不完整） */
        {
            Buf f;
            memset(&f, 0, sizeof(f));
            build_png(&f, &base, rows);
            QL_CHECK_EQ(decode_rgba(f.p, f.n / 2u, &img), QL_ERR_INVAL);
            free(f.p);
        }
        /* 5.10 zlib 头 FCHECK 不对 */
        {
            Buf f;
            memset(&f, 0, sizeof(f));
            build_png(&f, &base, rows);
            /* 找到第一个 IDAT 的数据起点：签名 8 + IHDR 25 + IDAT 头 8 */
            const size_t z0 = 8u + 25u + 8u;
            f.p[z0 + 1u] ^= 0x01u;      /* 破坏 FCHECK */
            QL_CHECK_EQ(decode_rgba(f.p, f.n, &img), QL_ERR_INVAL);
            free(f.p);
        }
        /* 5.11 声明了 FDICT */
        {
            Buf f;
            memset(&f, 0, sizeof(f));
            build_png(&f, &base, rows);
            const size_t z0 = 8u + 25u + 8u;
            /* 找一个 FLG 使 FDICT=1 且 FCHECK 正确：FLG 每次 +1 直到 %31==0 */
            for (uint32_t flg = 0x20; flg < 0x40; flg++) {
                if (((0x78u << 8) | flg) % 31u == 0u) {
                    f.p[z0 + 1u] = (uint8_t)flg;
                    break;
                }
            }
            QL_CHECK_EQ(decode_rgba(f.p, f.n, &img), QL_ERR_UNSUPPORTED);
            free(f.p);
        }
        /* 5.12 Adler-32 不对（改最后一个像素字节） */
        {
            Buf f;
            memset(&f, 0, sizeof(f));
            build_png(&f, &base, rows);
            /* IDAT 数据尾部 4 字节是 Adler-32 */
            const size_t z0 = 8u + 25u + 8u;
            const uint32_t idat_len = ((uint32_t)f.p[8u + 25u] << 24) |
                                      ((uint32_t)f.p[8u + 25u + 1] << 16) |
                                      ((uint32_t)f.p[8u + 25u + 2] << 8) |
                                      (uint32_t)f.p[8u + 25u + 3];
            const size_t adler_at = z0 + idat_len - 4u;
            f.p[adler_at] ^= 0xFFu;
            QL_CHECK_EQ(decode_rgba(f.p, f.n, &img), QL_ERR_INVAL);
            free(f.p);
        }
        /* 5.13 声称的行数比实际多（deflate 输出不够） */
        {
            PngSpec sp = base;
            sp.h = 8;                   /* 实际只给了 2 行 */
            Buf f;
            memset(&f, 0, sizeof(f));
            build_png(&f, &sp, rows);   /* g_png_raw_len 仍是 sizeof(rows) */
            QL_CHECK_EQ(decode_rgba(f.p, f.n, &img), QL_ERR_INVAL);
            free(f.p);
        }
        /* 5.14 声称的行数比实际少（deflate 输出过量） */
        {
            PngSpec sp = base;
            sp.h = 1;
            Buf f;
            memset(&f, 0, sizeof(f));
            build_png(&f, &sp, rows);
            QL_CHECK_EQ(decode_rgba(f.p, f.n, &img), QL_ERR_INVAL);
            free(f.p);
        }
        free(file.p);
    }

    /* ---------- 6. 输出到 RGB565 / A8 也走同一条路 ---------- */
    {
        const int W = 2, H = 2;
        uint8_t rows[2 * (2 * 3 + 1)];
        memset(rows, 0, sizeof(rows));
        /* 左上红 / 右上绿 / 左下蓝 / 右下白，filter 全 0 */
        rows[0] = 0;
        rows[1] = 255; rows[2] = 0;   rows[3] = 0;
        rows[4] = 0;   rows[5] = 255; rows[6] = 0;
        rows[7] = 0;
        rows[8] = 0;   rows[9] = 0;   rows[10] = 255;
        rows[11] = 255; rows[12] = 255; rows[13] = 255;

        PngSpec sp;
        memset(&sp, 0, sizeof(sp));
        sp.w = W;
        sp.h = H;
        sp.depth = 8;
        sp.ctype = 2;
        sp.idat_parts = 1;
        g_png_raw_len = sizeof(rows);
        Buf file;
        memset(&file, 0, sizeof(file));
        build_png(&file, &sp, rows);

        QLImageDecodeReq req;
        memset(&req, 0, sizeof(req));
        req.data = file.p;
        req.len = file.n;
        req.src_format = QL_IMAGE_SRC_PNG;
        req.out_format = QL_IMAGE_RGB565;
        QLImage img;
        QL_CHECK_EQ(ql_image_decode(&req, &img), QL_OK);
        if (img.px != NULL) {
            QL_CHECK_EQ((int)img.format, (int)QL_IMAGE_RGB565);
            const uint16_t *q = (const uint16_t *)img.px;
            QL_CHECK_EQ((int)q[0], 0xF800);
            QL_CHECK_EQ((int)q[1], 0x07E0);
            QL_CHECK_EQ((int)q[2], 0x001F);
            QL_CHECK_EQ((int)q[3], 0xFFFF);
            free_img(&img);
        }

        req.out_format = QL_IMAGE_A8;
        QL_CHECK_EQ(ql_image_decode(&req, &img), QL_OK);
        if (img.px != NULL) {
            QL_CHECK_EQ((int)img.format, (int)QL_IMAGE_A8);
            QL_CHECK_EQ((int)img.px[0], 76);    /* 纯红 luma ≈ 76 */
            free_img(&img);
        }
        free(file.p);
    }

#endif  /* QL_FEAT_IMG_PNG && QL_FEAT_IMG_INFLATE */

QL_TEST_END()
