/**
 * @file    qlinflate.h
 * @brief   有界 DEFLATE 解压（RFC 1951）。PNG 的 IDAT 用它，`QL_FEAT_IMG_INFLATE` 可裁。
 *
 * 三条约束，都是为了"外来数据必须有界"：
 *   1. **不分配窗口**：输出缓冲本身就是窗口（回溯距离 ≤ 32768，而任何合法的
 *      回溯都指向已经写出的字节），所以额外 RAM 是 0 —— 栈上只有一个 608 字节
 *      的 Huffman 表 ×2 和 256 字节的输入暂存。
 *   2. **输出上界由调用方给**（`dst_cap`）：超了就是 `QLZ_ERR_SPACE`，不增长、
 *      不猜测"也许还装得下"。
 *   3. **输入是流式回调**：PNG 的 IDAT 会被拆成若干个 chunk，中间夹着 chunk 头，
 *      所以这里不假设压缩数据在一段连续内存里（零拷贝，不做拼接）。
 *
 * 没有 CRC 校验、没有 gzip/zlib 头（PNG 用的是裸 DEFLATE）。
 */
#ifndef QLINFLATE_H
#define QLINFLATE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    /* 读最多 max 字节到 dst，返回实际读到的字节数；**返回 0 只能表示"确实没有更多"**。
     * 出错时也返回 0（调用方随后会因为流不完整而失败）。 */
    size_t (*read)(void *ctx, uint8_t *dst, size_t max);
    void    *ctx;
} QLZSource;

#define QLZ_OK            0
#define QLZ_ERR_DATA    (-1)   /* 压缩流非法（坏码表、坏回溯、坏块类型…） */
#define QLZ_ERR_SPACE   (-2)   /* 输出超过 dst_cap */
#define QLZ_ERR_INCOMP  (-3)   /* 流在块结束前就用完了 */

/*
 * 解压到 `dst`（最多 `dst_cap` 字节）。成功时 `*out_len` 是实际写出的字节数，
 * 并且**不要求**正好填满 dst_cap（PNG 的行数据长度由 IHDR 决定，多余字节忽略）。
 */
int ql_inflate(const QLZSource *src, uint8_t *dst, size_t dst_cap, size_t *out_len);

#endif /* QLINFLATE_H */
