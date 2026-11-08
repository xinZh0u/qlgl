/**
 * @file    qlimage.h
 * @brief   位图：解码结果与解码入口。绘制层与界面层共享的唯一契约。
 *
 * 两条铁律（外来数据必须有界）：
 *   1. **不信任任何输入**：`w/h/stride/len/format` 进绘制层之前先过 `ql_image_validate()`；
 *   2. **溢出先除后乘**：`w * h * bpp` 是这类模块的经典漏洞，见 `ql_image_validate()`。
 *
 * 设计见 doc/图像管线设计.md。
 */
#ifndef QLIMAGE_H
#define QLIMAGE_H

#include <stddef.h>
#include <stdint.h>

#include "qlconfig.h"

/* 解码后像素的排布。与 QLColorFormat 刻意分开：
 *   前者是"图片缓冲"，后者是"显示表面"，转换只发生在 blit 里。 */
/*
 * 顺序有讲究：**RGBA8888 必须是 0**。
 *
 * 它是最常见的"解码输出"格式，而 `QLImageDecodeReq` 通常被 memset 清零后只填
 * 关心里几个字段 —— 让 0 表示 RGBA8888，"没指定就用 RGBA8888"就是自然行为。
 * 反过来（RGB565 = 0）会踩坑：memset 出来的 0 会被当成"要 RGB565"。
 */
typedef enum {
    QL_IMAGE_RGBA8888 = 0,   /* 4 B/px，**非预乘** */
    QL_IMAGE_RGB565   = 1,   /* 2 B/px */
    QL_IMAGE_RGB888   = 2,   /* 3 B/px */
    QL_IMAGE_A8       = 3,   /* 1 B/px，灰度/蒙版 */
} QLImageFormat;

/* 解码输出的默认格式（= 0，所以 memset 出来的请求就是"要 RGBA8888"） */
#define QL_IMAGE_OUT_DEFAULT  ((uint8_t)QL_IMAGE_RGBA8888)

/* 资源在固件里的**编码**形式（不是像素格式） */
typedef enum {
    QL_IMAGE_SRC_RAW  = 0,   /* 构建期已解码：像素按 pixel_format 排布 */
    QL_IMAGE_SRC_BMP  = 1,
    QL_IMAGE_SRC_QOI  = 2,
    QL_IMAGE_SRC_PNG  = 3,
    QL_IMAGE_SRC_AUTO = 0xFF /* 运行期探测魔数（RAW 无法探测，必须显式给） */
} QLImageSrcFormat;

typedef struct {
    const uint8_t *px;       /* 有效数据；NULL = 未解码 / 解码失败 */
    uint32_t       len;      /* 字节数 = stride * h */
    uint16_t       w, h;
    uint16_t       stride;   /* 字节/行（可大于 w*bpp，便于对齐） */
    uint8_t        format;   /* QLImageFormat */
    uint8_t        owned;    /* 1 = 归调用方（QLUIRoot）释放；0 = 指向 .rodata */
} QLImage;

/* 每个像素的字节数；未知格式返回 0 */
uint32_t ql_image_format_bpp(uint8_t format);

/*
 * 校验一张图的自洽性与上界。返回 1 = 可用。
 * 检查项：format 合法、w/h 非 0、像素数 ≤ QL_CFG_IMAGE_MAX_PIXELS、
 * stride ≥ w*bpp、len == stride*h、stride 能装进 uint16。
 */
int ql_image_validate(const QLImage *img);

/*
 * 解码一段字节到 `out`（`out->px` 由本函数分配，`owned = 1`）。
 * 目前只有 RAW 直通；BMP/QOI/PNG 随开关逐个落地（见设计文档 §12）。
 *
 * `src_format` 传 QL_IMAGE_SRC_AUTO 时按魔数探测。
 * `raw_format` / `raw_w` / `raw_h` 只在 RAW 时使用。
 */
typedef struct {
    const uint8_t *data;
    size_t         len;
    uint8_t        src_format;   /* QLImageSrcFormat；AUTO = 按魔数探测 */
    uint8_t        pixel_format; /* **输入**是 RAW 时的像素格式（其他格式忽略） */
    uint8_t        out_format;   /* 想要的输出格式；0 = RGBA8888（见 QLImageFormat 顺序） */
    uint16_t       w, h;         /* **输入**是 RAW 时的尺寸（其他格式由文件头决定） */
} QLImageDecodeReq;

/* 返回 QL_OK / QL_ERR_INVAL / QL_ERR_NOMEM / QL_ERR_UNSUPPORTED */
int ql_image_decode(const QLImageDecodeReq *req, QLImage *out);

#endif /* QLIMAGE_H */
