/**
 * @file    test_png_fuzz.c
 * @brief   PNG 解码器的 fuzz：随机字节 / 截断 / 位翻转**只允许"返回失败"**，
 *          不许崩溃、不许越界读写、不许内存泄漏（配合 ASan + UBSan 跑）。
 *
 * 判据只有三条：
 *   1. 返回值必须是 `QL_OK` 或已知的错误码（不会返回随机值）；
 *   2. 返回 `QL_OK` 时解码结果必须过 `ql_image_validate()`；
 *   3. 进程不崩、不泄漏（后者由 ASan 的 leak 检测看）。
 *
 * 种子固定 → 可复现。语料来自 `test/assets/png` 里的真实小图（含动态 Huffman），
 * 再加纯随机字节。本轮不追求覆盖率，只追求"坏数据不越界"。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ql_test.h"

#ifndef QL_PNG_ASSET_DIR
#define QL_PNG_ASSET_DIR "test/assets/png"
#endif

/* xorshift32：固定种子，跨平台一致 */
static uint32_t s_rnd = 0x9E3779B9u;

static uint32_t rnd(void)
{
    s_rnd ^= s_rnd << 13;
    s_rnd ^= s_rnd >> 17;
    s_rnd ^= s_rnd << 5;
    return s_rnd;
}

static uint32_t rnd_below(uint32_t n)
{
    return (n == 0) ? 0 : (rnd() % n);
}

static uint8_t *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    if (sz <= 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (buf == NULL || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)sz;
    return buf;
}

/* 解一次并检查不变量；返回 1 = 通过了本轮的判据 */
static int try_decode(const uint8_t *data, size_t len)
{
    QLImageDecodeReq req;
    memset(&req, 0, sizeof(req));
    req.data = data;
    req.len = len;
    req.src_format = QL_IMAGE_SRC_AUTO;
    /* 一半的轮次换个输出格式，覆盖 RGB565/A8 的写入路径 */
    req.out_format = (uint8_t)(rnd() & 1u) ? QL_IMAGE_RGB565 : QL_IMAGE_RGBA8888;

    QLImage img;
    const int rc = ql_image_decode(&req, &img);
    if (rc == QL_OK) {
        if (!ql_image_validate(&img)) {
            printf("  [不变量] 返回 OK 但 ql_image_validate 拒绝\n");
            if (img.owned)
                ql_mem_free((void *)img.px);
            return 0;
        }
        if (img.owned)
            ql_mem_free((void *)img.px);
        return 1;
    }
    /* 失败必须是已知的负值错误码 */
    if (rc >= 0) {
        printf("  [不变量] 失败路径返回了 %d\n", rc);
        return 0;
    }
    return 1;
}

QL_TEST_BEGIN("test_png_fuzz")

#if QL_FEAT_IMG_PNG && QL_FEAT_IMG_INFLATE
    /* 语料：几份真实的、能被正确解出来的小 PNG */
    static const char *const k_seeds[] = { "rgb8_quad", "pal4_blocks", "rgba8_sprite",
                                           "bw1_checker" };
    size_t nseed = sizeof(k_seeds) / sizeof(k_seeds[0]);
    uint8_t *seed[4];
    size_t seed_len[4];
    size_t have = 0;
    for (size_t i = 0; i < nseed; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.png", QL_PNG_ASSET_DIR, k_seeds[i]);
        seed[have] = read_file(path, &seed_len[have]);
        if (seed[have] != NULL)
            have++;
    }
    QL_CHECK(have > 0);

    int bad = 0;

    /* ---- 1. 纯随机字节（一半带 PNG 签名，好走进容器解析） ---- */
    for (int it = 0; it < 1500 && bad == 0; it++) {
        const size_t len = 1u + rnd_below(600u);
        uint8_t *buf = (uint8_t *)malloc(len);
        if (buf == NULL) {
            bad = 1;
            break;
        }
        for (size_t i = 0; i < len; i++)
            buf[i] = (uint8_t)rnd();
        if ((rnd() & 1u) != 0 && len >= 8u)
            memcpy(buf, "\x89PNG\r\n\x1a\n", 8);
        if (!try_decode(buf, len))
            bad = 1;
        free(buf);
    }
    QL_CHECK_EQ(bad, 0);

    /* ---- 2. 真实语料的变异：截断 / 位翻转 / 插删字节 / 覆写 4 字节 ---- */
    for (int it = 0; it < 3000 && bad == 0 && have > 0; it++) {
        const size_t s = rnd_below((uint32_t)have);
        const size_t len = seed_len[s];
        uint8_t *buf = (uint8_t *)malloc(len + 8u);
        if (buf == NULL) {
            bad = 1;
            break;
        }
        size_t n = len;
        memcpy(buf, seed[s], len);

        const int kind = (int)rnd_below(4u);
        if (kind == 0) {                     /* 截断 */
            n = (size_t)rnd_below((uint32_t)len);
        } else if (kind == 1) {              /* 位翻转 1..8 处 */
            const uint32_t flips = 1u + rnd_below(8u);
            for (uint32_t f = 0; f < flips; f++)
                buf[rnd_below((uint32_t)n)] ^= (uint8_t)(1u << (rnd() & 7u));
        } else if (kind == 2) {              /* 覆写一个 4 字节字（多半打断长度字段） */
            const size_t at = rnd_below((uint32_t)n);
            const uint32_t v = rnd();
            for (size_t k = 0; k < 4u && at + k < n; k++)
                buf[at + k] = (uint8_t)(v >> (8u * (uint32_t)k));
        } else {                             /* 在随机位置插入一个随机字节 */
            const size_t at = rnd_below((uint32_t)n);
            memmove(buf + at + 1u, buf + at, n - at);
            buf[at] = (uint8_t)rnd();
            n++;
        }
        if (!try_decode(buf, n))
            bad = 1;
        free(buf);
    }
    QL_CHECK_EQ(bad, 0);

    /* ---- 3. 有界：头部声称巨大尺寸（不分配，直接拒绝） ---- */
    for (int it = 0; it < 200 && bad == 0 && have > 0; it++) {
        const size_t s = rnd_below((uint32_t)have);
        const size_t len = seed_len[s];
        uint8_t *buf = (uint8_t *)malloc(len);
        if (buf == NULL) {
            bad = 1;
            break;
        }
        memcpy(buf, seed[s], len);
        if (len >= 8u + 8u + 8u) {
            /* IHDR 的宽高在大端 8..15 字节处 */
            const uint32_t big = 0xFFFFu;
            buf[8 + 8] = (uint8_t)(big >> 24);
            buf[8 + 9] = (uint8_t)(big >> 16);
            buf[8 + 10] = (uint8_t)(big >> 8);
            buf[8 + 11] = (uint8_t)big;
            buf[8 + 12] = (uint8_t)(big >> 24);
            buf[8 + 13] = (uint8_t)(big >> 16);
            buf[8 + 14] = (uint8_t)(big >> 8);
            buf[8 + 15] = (uint8_t)big;
        }
        if (!try_decode(buf, len))
            bad = 1;
        free(buf);
    }
    QL_CHECK_EQ(bad, 0);

    for (size_t i = 0; i < have; i++)
        free(seed[i]);
#else
    QL_CHECK(1);      /* 这个构建没有 PNG：没什么可 fuzz 的 */
#endif

QL_TEST_END()
