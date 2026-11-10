/**
 * @file    qllayer.h
 * @brief   离屏图层的像素运算：反解 alpha、滤镜链、drop-shadow、合成。
 *
 * 这一份是**后端无关的纯缓冲运算**（只有 CPU 那侧需要；GPU 在着色器里做同一套
 * 数学）。放进独立文件是因为它的测试面很窄、很密：每一行都能用像素断言验证，
 * 而混在 qlcanvas.c / qldraw2d.c 里会被光栅代码淹掉。
 *
 * 缓冲约定：`rgba` 是 **w*h*4 的预乘（premultiplied）RGBA**，一行 w*4 字节，
 * 与 QL_COLOR_ARGB8888 的字节序一致（内存里 R,G,B,A）。
 * 预乘的理由：src-over 在预乘空间只是 `dst = src + dst*(1-a)`，
 * 模糊/缩放也不会在透明边缘渗出黑边。
 */
#ifndef QLGL_LAYER_H
#define QLGL_LAYER_H

#include "qlgl/qlgl.h"
#include "qlgl/qlcanvas.h"
#include "qlgl/qldraw2d.h"

/*
 * 由"黑底 / 白底"两遍渲染的结果反解出预乘 RGBA。
 *
 * 原理：设某个像素最终是 px-over 结果，白色内容的 alpha = a、颜色 = C，那么
 *   黑底上：B = C*a
 *   白底上：W = C*a + 255*(1-a)
 * 于是 a = 255 - (W - B)，而 B 本身就是**预乘后的颜色**（= C*a）。
 * 所以既不需要除 alpha（除法的量化误差很大），也不需要知道 C。
 *
 * `black/white` 都是 w*h*3 的紧凑 RGB888。
 */
void ql_layer_reconstruct(const uint8_t *black, const uint8_t *white, int w, int h,
                          uint8_t *rgba);

/*
 * 就地应用 `fx->op[]` 里的颜色矩阵族（brightness / contrast / saturate /
 * grayscale / sepia / invert / hue-rotate / opacity）。`blur` / `drop-shadow`
 * 不在这里做（它们要改尺寸 / 要走模糊，见下面两个函数）。
 */
void ql_layer_apply_color_ops(uint8_t *rgba, int w, int h,
                              const QLDraw2DLayerFx *fx);

/*
 * 就地做 `drop-shadow()`：抽出 alpha → 偏移 → 模糊 → 染色 → 叠在图层**下面**。
 * 调用方必须已经把 `bounds` 外扩出 (|dx|, |dy|, 3*blur) 的余量，
 * 否则阴影会被裁掉一角（这里不做边界增长）。
 */
void ql_layer_drop_shadow(uint8_t *rgba, int w, int h, const QLDraw2DLayerFx *fx);

/* 就地高斯（预乘 RGBA，4 通道一起糊；半径单位：像素） */
void ql_layer_blur(uint8_t *rgba, int w, int h, int radius);

/*
 * 合成到目标画布 `dst` 的 `at`（设备像素）位置：
 *   - `fx->opacity` 乘进 alpha；
 *   - `fx->blend` 决定与已有像素怎么混；
 *   - `fx->mask` 用图片的 alpha 再乘一次；
 *   - `fx->corner` 把结果裁进圆角矩形。
 */
void ql_layer_composite(QLCanvas *dst, const uint8_t *rgba, int w, int h, QLRect at,
                        const QLDraw2DLayerFx *fx);

/* 单个像素点的圆角覆盖率（0..255），合成时用；与 qlcanvas 的圆角同一公式 */
uint32_t ql_layer_round_cov(QLRect box, ql_coord_t corner, ql_coord_t x, ql_coord_t y);

#endif /* QLGL_LAYER_H */
