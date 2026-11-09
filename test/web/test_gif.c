/**
 * @file    test_gif.c
 * @brief   GIF 首帧解码：LZW、调色板、透明度、隔行、画布偏移、坏数据。
 *
 * 输入是**测试里现场构造**的 GIF —— 自带一个极简 LZW 编码器，所以
 * "喂进去的像素"完全可控，字典增长与 KwKwK（`code == next`）都能精确造出来。
 * Pillow 生成的真实文件另由 `test_gif_assets` 对拍。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ql_test.h"

/* ------------------------------ 字节缓冲 ------------------------------ */

typedef struct {
    uint8_t *p;
    size_t   n, cap;
} Buf;

static void bput(Buf *b, const void *d, size_t n)
{
    if (b->n + n > b->cap) {
        size_t cap = (b->cap > 0) ? b->cap : 256;
        while (cap < b->n + n)
            cap *= 2;
        uint8_t *np = (uint8_t *)realloc(b->p, cap);
        if (np == NULL)
            abort();
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}

static void bbyte(Buf *b, uint8_t v) { bput(b, &v, 1); }

static void ble16(Buf *b, uint16_t v)
{
    bbyte(b, (uint8_t)(v & 0xFF));
    bbyte(b, (uint8_t)(v >> 8));
}

/* ------------------------------ LZW 编码器 ------------------------------ */

/*
 * GIF LZW 编码器（测试专用）。码长增长与"字典满则重发 clear"都按规范来，
 * 这样解码器的 `next == (1<<code_size)` 与 KwKwK 两条路径都能被覆盖到。
 */
typedef struct {
    Buf     *out;        /* 子块数据（先攒成一段，最后再切块） */
    uint32_t acc;
    int      acc_bits;
    int      code_size;
    int      next;
    int      clear, end;
} LzwEnc;

static void lzw_put_code(LzwEnc *e, uint32_t code)
{
    e->acc |= code << e->acc_bits;
    e->acc_bits += e->code_size;
    while (e->acc_bits >= 8) {
        bbyte(e->out, (uint8_t)(e->acc & 0xFFu));
        e->acc >>= 8;
        e->acc_bits -= 8;
    }
}

/*
 * 发一个码：**先**按 next 决定码长，再写位。
 *
 * 判据是 `next == (1<<code_size) + 1` 而不是 `== (1<<code_size)`：
 * 解码器是"读完第 k 个码才加第 k-1 个表项"，编码器是"发第 k 个码之前已经加了
 * k-1 个"，两边差一项 —— 写成一样就会和真实解码器错开一个码，PIL 会直接报
 * "broken data stream"。
 */
static void lzw_emit(LzwEnc *e, uint32_t code)
{
    if (e->next == (1 << e->code_size) + 1 && e->code_size < 12)
        e->code_size++;
    lzw_put_code(e, code);
}

/* (prefix, ch) → 码。开放寻址，8192 槽（4096 项最坏负载 0.5） */
static int32_t s_key[8192];
static int16_t s_val[8192];

static int lzw_encode(const uint8_t *idx, size_t n, int min_size, Buf *out)
{
    Buf data;
    memset(&data, 0, sizeof(data));

    LzwEnc e;
    memset(&e, 0, sizeof(e));
    e.out = &data;
    e.clear = 1 << min_size;
    e.end = e.clear + 1;
    e.next = e.end + 1;
    e.code_size = min_size + 1;
    for (size_t i = 0; i < 8192; i++) {
        s_key[i] = -1;
        s_val[i] = 0;
    }

    if (n == 0) {
        lzw_emit(&e, (uint32_t)e.clear);
        lzw_emit(&e, (uint32_t)e.end);
    } else {
        lzw_emit(&e, (uint32_t)e.clear);
        int prefix = idx[0];
        for (size_t i = 1; i < n; i++) {
            const uint8_t ch = idx[i];
            const uint32_t want = (uint32_t)prefix * 256u + ch;
            uint32_t h = (want * 2654435761u) & 8191u;
            int found = -1;
            while (s_key[h] >= 0) {
                if ((uint32_t)s_key[h] == want) {
                    found = s_val[h];
                    break;
                }
                h = (h + 1u) & 8191u;
            }
            if (found >= 0) {
                prefix = found;
                continue;
            }
            lzw_emit(&e, (uint32_t)prefix);
            if (e.next < 4096) {
                uint32_t k = (want * 2654435761u) & 8191u;
                while (s_key[k] >= 0)
                    k = (k + 1u) & 8191u;
                s_key[k] = (int32_t)want;
                s_val[k] = (int16_t)e.next;
                e.next++;
            } else {
                /* 字典满：重发 clear，双方一起回到初始状态 */
                lzw_emit(&e, (uint32_t)e.clear);
                for (size_t k2 = 0; k2 < 8192; k2++) {
                    s_key[k2] = -1;
                    s_val[k2] = 0;
                }
                e.next = e.end + 1;
                e.code_size = min_size + 1;
            }
            prefix = ch;
        }
        lzw_emit(&e, (uint32_t)prefix);
        lzw_emit(&e, (uint32_t)e.end);
    }
    if (e.acc_bits > 0)
        bbyte(&data, (uint8_t)(e.acc & 0xFFu));

    /* 切成 ≤255 的子块 */
    size_t off = 0;
    while (off < data.n) {
        size_t take = data.n - off;
        if (take > 255u)
            take = 255u;
        bbyte(out, (uint8_t)take);
        bput(out, data.p + off, take);
        off += take;
    }
    bbyte(out, 0);                     /* 块终止 */
    free(data.p);
    return 1;
}

/* ------------------------------ GIF 构造器 ------------------------------ */

typedef struct {
    int      w, h;              /* 逻辑屏幕 */
    const uint8_t *gct;         /* 3*n */
    int      gct_n;             /* 必须是 2 的幂（2/4/8/…） */
    int      interlace;
    int      left, top;         /* 图像在画布里的偏移 */
    int      iw, ih;            /* 图像尺寸 */
    const uint8_t *lct;         /* 局部颜色表（可空） */
    int      lct_n;
    const uint8_t *idx;         /* iw*ih 个调色板索引（**未**隔行重排） */
    int      trans;             /* -1 = 不透明 */
    int      with_gce;          /* 0 = 只写 87a（没有图形控制扩展） */
    int      force_min_size;    /* >0 = 强行指定 LZW 最小码长（造越界索引用） */
    int      encode_n;          /* >0 = 只编码这么多个索引（造"像素不够"用） */
    const char *version;        /* "GIF87a" / "GIF89a" */
} GifSpec;

/* 把行序重排成隔行顺序（解码器负责反过来） */
static void interlace_rows(const uint8_t *idx, int w, int h, uint8_t *out)
{
    static const int start[4] = { 0, 4, 2, 1 };
    static const int step[4] = { 8, 8, 4, 2 };
    size_t o = 0;
    for (int pass = 0; pass < 4; pass++)
        for (int y = start[pass]; y < h; y += step[pass]) {
            memcpy(out + o, idx + (size_t)y * (size_t)w, (size_t)w);
            o += (size_t)w;
        }
}

static void build_gif(Buf *out, const GifSpec *sp)
{
    bput(out, sp->version, 6u);
    ble16(out, (uint16_t)sp->w);
    ble16(out, (uint16_t)sp->h);

    /* 全局颜色表：size 字段是 log2(n)-1 */
    int gct_bits = 0;
    while ((2 << gct_bits) < sp->gct_n)
        gct_bits++;
    uint8_t fl = (uint8_t)((sp->gct_n > 0 ? 0x80u : 0u) | (uint8_t)(gct_bits & 7));
    bbyte(out, fl);                    /* packed */
    bbyte(out, 0);                     /* 背景色索引 */
    bbyte(out, 0);                     /* 像素宽高比 */
    if (sp->gct_n > 0)
        bput(out, sp->gct, (size_t)sp->gct_n * 3u);

    if (sp->with_gce) {
        bbyte(out, 0x21);
        bbyte(out, 0xF9);
        bbyte(out, 4);                 /* 块长 */
        bbyte(out, (uint8_t)((sp->trans >= 0) ? 0x01u : 0x00u));
        ble16(out, 0);                 /* 延迟 */
        bbyte(out, (uint8_t)((sp->trans >= 0) ? sp->trans : 0));
        bbyte(out, 0);                 /* 终止符 */
    }

    bbyte(out, 0x2C);                  /* 图像描述符 */
    ble16(out, (uint16_t)sp->left);
    ble16(out, (uint16_t)sp->top);
    ble16(out, (uint16_t)sp->iw);
    ble16(out, (uint16_t)sp->ih);
    const int lct_n = (sp->lct_n > 0) ? sp->lct_n : sp->gct_n;
    int lct_bits = 0;
    while ((2 << lct_bits) < lct_n)
        lct_bits++;
    bbyte(out, (uint8_t)((sp->lct_n > 0 ? 0x80u : 0u) |
                         (sp->interlace ? 0x40u : 0u) | (uint8_t)(lct_bits & 7)));
    if (sp->lct_n > 0)
        bput(out, sp->lct, (size_t)sp->lct_n * 3u);

    /* LZW 最小码长：至少要能表示调色板大小 */
    int min_size = sp->force_min_size;
    if (min_size <= 0) {
        min_size = 2;
        while ((1 << min_size) < lct_n && min_size < 8)
            min_size++;
    }
    bbyte(out, (uint8_t)min_size);

    uint8_t *ordered = NULL;
    const uint8_t *src = sp->idx;
    if (sp->interlace) {
        ordered = (uint8_t *)malloc((size_t)sp->iw * (size_t)sp->ih);
        if (ordered == NULL)
            abort();
        interlace_rows(sp->idx, sp->iw, sp->ih, ordered);
        src = ordered;
    }
    {
        const size_t total = (size_t)sp->iw * (size_t)sp->ih;
        const size_t enc = (sp->encode_n > 0 && (size_t)sp->encode_n < total)
                               ? (size_t)sp->encode_n
                               : total;
        lzw_encode(src, enc, min_size, out);
    }
    free(ordered);

    bbyte(out, 0x3B);                  /* trailer */
}

/* ------------------------------ 断言工具 ------------------------------ */

static int decode_bytes(const uint8_t *d, size_t n, uint8_t out_format, QLImage *img)
{
    QLImageDecodeReq req;
    memset(&req, 0, sizeof(req));
    req.data = d;
    req.len = n;
    req.src_format = QL_IMAGE_SRC_AUTO;
    req.out_format = out_format;
    return ql_image_decode(&req, img);
}

static void expect_rgba(const QLImage *img, int x, int y, int r, int g, int b, int a)
{
    const uint8_t *p = img->px + (size_t)y * img->stride + (size_t)x * 4u;
    QL_CHECK_EQ((int)p[0], r);
    QL_CHECK_EQ((int)p[1], g);
    QL_CHECK_EQ((int)p[2], b);
    QL_CHECK_EQ((int)p[3], a);
}

static void free_img(QLImage *img)
{
    if (img->owned && img->px != NULL)
        ql_mem_free((void *)img->px);
}

/* 四色调色板：红 / 绿 / 蓝 / 白 */
static const uint8_t k_pal4[4 * 3] = {
    255, 0,   0,
    0,   255, 0,
    0,   0,   255,
    255, 255, 255,
};

QL_TEST_BEGIN("test_gif")

#if !QL_FEAT_IMG_GIF
    {
        static const uint8_t sig[6] = { 'G', 'I', 'F', '8', '9', 'a' };
        QLImage img;
        QL_CHECK_EQ(decode_bytes(sig, sizeof(sig), 0, &img), QL_ERR_UNSUPPORTED);
    }
#else
    /* ---------- 1. 4×4 四色，逐像素 ---------- */
    {
        uint8_t idx[16];
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                idx[y * 4 + x] = (uint8_t)(((x / 2) + (y / 2) * 2) & 3);

        GifSpec sp;
        memset(&sp, 0, sizeof(sp));
        sp.w = sp.iw = 4;
        sp.h = sp.ih = 4;
        sp.gct = k_pal4;
        sp.gct_n = 4;
        sp.idx = idx;
        sp.trans = -1;
        sp.with_gce = 0;
        sp.version = "GIF87a";

        Buf f;
        memset(&f, 0, sizeof(f));
        build_gif(&f, &sp);

        QLImage img;
        QL_CHECK_EQ(decode_bytes(f.p, f.n, 0, &img), QL_OK);
        if (img.px != NULL) {
            QL_CHECK_EQ((int)img.w, 4);
            QL_CHECK_EQ((int)img.h, 4);
            expect_rgba(&img, 0, 0, 255, 0, 0, 255);       /* 红 */
            expect_rgba(&img, 3, 0, 0, 255, 0, 255);       /* 绿 */
            expect_rgba(&img, 0, 3, 0, 0, 255, 255);       /* 蓝 */
            expect_rgba(&img, 3, 3, 255, 255, 255, 255);   /* 白 */
            free_img(&img);
        }
        free(f.p);
    }

    /* ---------- 2. 隔行扫描：同样的索引流，行序必须还原 ---------- */
    {
        uint8_t idx[6 * 7];
        for (int y = 0; y < 7; y++)
            for (int x = 0; x < 6; x++)
                idx[y * 6 + x] = (uint8_t)(y & 3);

        GifSpec sp;
        memset(&sp, 0, sizeof(sp));
        sp.w = sp.iw = 6;
        sp.h = sp.ih = 7;
        sp.gct = k_pal4;
        sp.gct_n = 4;
        sp.idx = idx;
        sp.interlace = 1;
        sp.trans = -1;
        sp.version = "GIF87a";

        Buf f;
        memset(&f, 0, sizeof(f));
        build_gif(&f, &sp);

        QLImage img;
        QL_CHECK_EQ(decode_bytes(f.p, f.n, 0, &img), QL_OK);
        if (img.px != NULL) {
            /* 每一行都应该是同一个颜色（按 y&3 变化），逐行验证行序没错位 */
            for (int y = 0; y < 7; y++) {
                const int ci = y & 3;
                expect_rgba(&img, 2, y, k_pal4[ci * 3], k_pal4[ci * 3 + 1],
                            k_pal4[ci * 3 + 2], 255);
            }
            free_img(&img);
        }
        free(f.p);
    }

    /* ---------- 3. 透明色：未覆盖处 alpha = 0 ---------- */
    {
        uint8_t idx[4] = { 0, 1, 2, 3 };
        GifSpec sp;
        memset(&sp, 0, sizeof(sp));
        sp.w = sp.iw = 2;
        sp.h = sp.ih = 2;
        sp.gct = k_pal4;
        sp.gct_n = 4;
        sp.idx = idx;
        sp.trans = 1;                  /* 索引 1 透明 */
        sp.with_gce = 1;
        sp.version = "GIF89a";

        Buf f;
        memset(&f, 0, sizeof(f));
        build_gif(&f, &sp);

        QLImage img;
        QL_CHECK_EQ(decode_bytes(f.p, f.n, 0, &img), QL_OK);
        if (img.px != NULL) {
            expect_rgba(&img, 0, 0, 255, 0, 0, 255);
            expect_rgba(&img, 1, 0, 0, 0, 0, 0);           /* 透明 */
            expect_rgba(&img, 0, 1, 0, 0, 255, 255);
            free_img(&img);
        }
        free(f.p);
    }

    /* ---------- 4. 画布大于图像：未覆盖区域全透明，位置按 left/top ---------- */
    {
        uint8_t idx[4] = { 3, 3, 3, 3 };   /* 全白 */
        GifSpec sp;
        memset(&sp, 0, sizeof(sp));
        sp.w = 6;
        sp.h = 5;
        sp.left = 2;
        sp.top = 1;
        sp.iw = 2;
        sp.ih = 2;
        sp.gct = k_pal4;
        sp.gct_n = 4;
        sp.idx = idx;
        sp.trans = -1;
        sp.version = "GIF87a";

        Buf f;
        memset(&f, 0, sizeof(f));
        build_gif(&f, &sp);

        QLImage img;
        QL_CHECK_EQ(decode_bytes(f.p, f.n, 0, &img), QL_OK);
        if (img.px != NULL) {
            QL_CHECK_EQ((int)img.w, 6);        /* 逻辑屏幕尺寸，不是图像尺寸 */
            QL_CHECK_EQ((int)img.h, 5);
            expect_rgba(&img, 0, 0, 0, 0, 0, 0);        /* 画布内、图像外 */
            expect_rgba(&img, 2, 1, 255, 255, 255, 255); /* 左上角落在 (2,1) */
            expect_rgba(&img, 3, 2, 255, 255, 255, 255);
            expect_rgba(&img, 4, 3, 0, 0, 0, 0);
            free_img(&img);
        }
        free(f.p);
    }

    /* ---------- 5. 局部颜色表盖过全局 ---------- */
    {
        static const uint8_t lct[4 * 3] = { 10, 20, 30, 40, 50, 60, 70, 80, 90, 1, 2, 3 };
        uint8_t idx[4] = { 0, 1, 2, 3 };
        GifSpec sp;
        memset(&sp, 0, sizeof(sp));
        sp.w = sp.iw = 2;
        sp.h = sp.ih = 2;
        sp.gct = k_pal4;
        sp.gct_n = 4;
        sp.lct = lct;
        sp.lct_n = 4;
        sp.idx = idx;
        sp.trans = -1;
        sp.version = "GIF87a";

        Buf f;
        memset(&f, 0, sizeof(f));
        build_gif(&f, &sp);

        QLImage img;
        QL_CHECK_EQ(decode_bytes(f.p, f.n, 0, &img), QL_OK);
        if (img.px != NULL) {
            expect_rgba(&img, 0, 0, 10, 20, 30, 255);
            expect_rgba(&img, 1, 0, 40, 50, 60, 255);
            expect_rgba(&img, 1, 1, 1, 2, 3, 255);
            free_img(&img);
        }
        free(f.p);
    }

    /* ---------- 6. 256 色调色板 + 长索引流（字典会跨码长增长） ---------- */
    {
        static uint8_t pal[256 * 3];
        for (int i = 0; i < 256; i++) {
            pal[i * 3] = (uint8_t)i;
            pal[i * 3 + 1] = (uint8_t)(255 - i);
            pal[i * 3 + 2] = (uint8_t)(i * 7);
        }
        const int W = 40, H = 30;
        uint8_t *idx = (uint8_t *)malloc((size_t)W * (size_t)H);
        QL_CHECK(idx != NULL);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                idx[y * W + x] = (uint8_t)((x * 3 + y * 5) & 0xFF);

        GifSpec sp;
        memset(&sp, 0, sizeof(sp));
        sp.w = sp.iw = W;
        sp.h = sp.ih = H;
        sp.gct = pal;
        sp.gct_n = 256;
        sp.idx = idx;
        sp.trans = -1;
        sp.version = "GIF87a";

        Buf f;
        memset(&f, 0, sizeof(f));
        build_gif(&f, &sp);

        QLImage img;
        QL_CHECK_EQ(decode_bytes(f.p, f.n, 0, &img), QL_OK);
        if (img.px != NULL) {
            int bad = 0;
            for (int y = 0; y < H && bad == 0; y++)
                for (int x = 0; x < W && bad == 0; x++) {
                    const int ci = (x * 3 + y * 5) & 0xFF;
                    const uint8_t *p = img.px + (size_t)y * img.stride + (size_t)x * 4u;
                    if (p[0] != pal[ci * 3] || p[1] != pal[ci * 3 + 1] ||
                        p[2] != pal[ci * 3 + 2] || p[3] != 255)
                        bad = 1;
                }
            QL_CHECK_EQ(bad, 0);
            free_img(&img);
        }
        free(f.p);
        free(idx);
    }

    /* ---------- 7. 输出 RGB565 / A8 ---------- */
    {
        uint8_t idx[4] = { 0, 1, 2, 3 };
        GifSpec sp;
        memset(&sp, 0, sizeof(sp));
        sp.w = sp.iw = 2;
        sp.h = sp.ih = 2;
        sp.gct = k_pal4;
        sp.gct_n = 4;
        sp.idx = idx;
        sp.trans = -1;
        sp.version = "GIF87a";

        Buf f;
        memset(&f, 0, sizeof(f));
        build_gif(&f, &sp);

        QLImage img;
        QL_CHECK_EQ(decode_bytes(f.p, f.n, QL_IMAGE_RGB565, &img), QL_OK);
        if (img.px != NULL) {
            QL_CHECK_EQ((int)img.format, (int)QL_IMAGE_RGB565);
            const uint16_t *q = (const uint16_t *)img.px;
            QL_CHECK_EQ((int)q[0], 0xF800);
            QL_CHECK_EQ((int)q[1], 0x07E0);
            QL_CHECK_EQ((int)q[2], 0x001F);
            QL_CHECK_EQ((int)q[3], 0xFFFF);
            free_img(&img);
        }
        QL_CHECK_EQ(decode_bytes(f.p, f.n, QL_IMAGE_A8, &img), QL_OK);
        if (img.px != NULL) {
            QL_CHECK_EQ((int)img.px[0], 76);      /* 纯红 luma */
            free_img(&img);
        }
        free(f.p);
    }

    /* ---------- 8. 明确失败 ---------- */
    {
        uint8_t idx[4] = { 0, 1, 2, 3 };
        GifSpec base;
        memset(&base, 0, sizeof(base));
        base.w = base.iw = 2;
        base.h = base.ih = 2;
        base.gct = k_pal4;
        base.gct_n = 4;
        base.idx = idx;
        base.trans = -1;
        base.version = "GIF87a";

        Buf good;
        memset(&good, 0, sizeof(good));
        build_gif(&good, &base);
        QLImage img;
        QL_CHECK_EQ(decode_bytes(good.p, good.n, 0, &img), QL_OK);
        if (img.px != NULL)
            free_img(&img);

        /* 8.1 签名不对 */
        QL_CHECK_EQ(decode_bytes(good.p + 1, good.n - 1u, 0, &img),
                    QL_ERR_UNSUPPORTED);
        /*
         * 8.2 截断：切在数据段中间必须失败；只砍掉尾部（块终止符/trailer）时
         * 像素已经完整，允许成功 —— 浏览器也是这样。
         */
        for (size_t cut = 6; cut < good.n / 2u; cut += 5) {
            const int rc = decode_bytes(good.p, cut, 0, &img);
            QL_CHECK(rc != QL_OK);
            if (rc == QL_OK && img.owned)
                ql_mem_free((void *)img.px);
        }
        /* 8.3 没有图像（只有 header + trailer） */
        {
            Buf f;
            memset(&f, 0, sizeof(f));
            bput(&f, "GIF89a", 6);
            ble16(&f, 1);
            ble16(&f, 1);
            bbyte(&f, 0x00);
            bbyte(&f, 0);
            bbyte(&f, 0);
            bbyte(&f, 0x3B);
            QL_CHECK_EQ(decode_bytes(f.p, f.n, 0, &img), QL_ERR_INVAL);
            free(f.p);
        }
        /* 8.4 逻辑屏幕超过像素上限 */
        {
            Buf f;
            memset(&f, 0, sizeof(f));
            bput(&f, "GIF89a", 6);
            ble16(&f, 0xFFFF);
            ble16(&f, 0xFFFF);
            bbyte(&f, 0x00);
            bbyte(&f, 0);
            bbyte(&f, 0);
            QL_CHECK_EQ(decode_bytes(f.p, f.n, 0, &img), QL_ERR_INVAL);
            free(f.p);
        }
        /* 8.5 像素不够（图像尺寸写大，索引流只有 4 个） */
        {
            GifSpec sp = base;
            sp.iw = 8;
            sp.ih = 8;
            sp.encode_n = 4;            /* 描述符说 8×8，索引流只有 4 个 */
            Buf f;
            memset(&f, 0, sizeof(f));
            build_gif(&f, &sp);
            QL_CHECK_EQ(decode_bytes(f.p, f.n, 0, &img), QL_ERR_INVAL);
            free(f.p);
        }
        /* 8.6 调色板索引越界：留透明，不猜颜色 */
        {
            uint8_t bad_idx[2] = { 0, 9 };    /* 调色板只有 4 个 */
            GifSpec sp = base;
            sp.w = 2;
            sp.h = 1;
            sp.iw = 2;
            sp.ih = 1;
            sp.idx = bad_idx;
            sp.force_min_size = 8;    /* 让 9 成为合法字面码，但调色板只有 4 色 */
            Buf f;
            memset(&f, 0, sizeof(f));
            build_gif(&f, &sp);
            QL_CHECK_EQ(decode_bytes(f.p, f.n, 0, &img), QL_OK);
            if (img.px != NULL) {
                expect_rgba(&img, 0, 0, 255, 0, 0, 255);
                expect_rgba(&img, 1, 0, 0, 0, 0, 0);   /* 越界 → 透明 */
                free_img(&img);
            }
            free(f.p);
        }
        free(good.p);
    }
#endif

QL_TEST_END()
