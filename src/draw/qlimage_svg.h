/**
 * @file    qlimage_svg.h
 * @brief   运行期 SVG 解码（内部接口，不安装）。
 */
#ifndef QLGL_SRC_DRAW_QLIMAGE_SVG_H
#define QLGL_SRC_DRAW_QLIMAGE_SVG_H

#include <stddef.h>
#include <stdint.h>

#include <qlgl/qlimage.h>

/*
 * 把 SVG 文本渲染成位图（`QL_FEAT_IMG_SVG=1` 时才编译）。
 * 成功返回 `QL_OK`，`out->owned = 1`；失败返回错误码，并在 `err` 里写原因。
 */
int ql_image_decode_svg(const QLImageDecodeReq *req, QLImage *out,
                        uint8_t out_format, char *err, size_t err_cap);

#endif /* QLGL_SRC_DRAW_QLIMAGE_SVG_H */
