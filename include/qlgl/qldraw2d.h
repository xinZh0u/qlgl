/**
 * @file    qldraw2d.h
 * @brief   2D 绘制接口：CPU 光栅与 GPU 后端共用的唯一分界。
 *
 * 上层（qlui 布局/控件）只认识这个接口，因此同一份界面可以：
 *   - 用 CPU 画到帧缓冲（MCU / 无 GPU / 离屏导出）
 *   - 用 GPU 画到窗口（PC / 移动）
 * 两个后端的输出必须一致，用 golden image 测试守住。
 *
 * 坐标一律是"目标表面的像素坐标"，原点左上，y 向下。
 */
#ifndef QLGL_DRAW2D_H
#define QLGL_DRAW2D_H

#include "qlgl.h"
#include "qlgeom.h"
#include "qlfont.h"
#include "qlcanvas.h"

typedef struct QLDraw2D QLDraw2D;

struct QLDraw2D {
    /* 设置裁剪矩形（绝对坐标）；之后的绘制都被裁到它里面 */
    void (*set_clip)(QLDraw2D *self, QLRect clip);

    /* alpha: 0 透明（跳过），255 不透明 */
    void (*fill_rect)(QLDraw2D *self, QLRect rect, ql_color_t color, uint8_t alpha);
    void (*fill_round_rect)(QLDraw2D *self, QLRect rect, ql_coord_t radius,
                            ql_color_t color, uint8_t alpha);
    /* 描边在矩形内侧 */
    void (*stroke_round_rect)(QLDraw2D *self, QLRect rect, ql_coord_t radius,
                              ql_coord_t thickness, ql_color_t color);

    /*
     * 线性渐变圆角矩形：from → to，dir 见 QLGradientDir。
     * GPU 侧用"顶点颜色插值"实现，所以不增加顶点、不增加 draw call。
     */
    void (*fill_round_rect_gradient)(QLDraw2D *self, QLRect rect, ql_coord_t radius,
                                     ql_color_t from, ql_color_t to, uint8_t dir,
                                     uint8_t alpha);
    /*
     * 软阴影：以 inner（圆角 radius）为轮廓，向外扩散 blur，整体偏移 (dx,dy)。
     * 调用方必须**先画阴影再画本体**。
     */
    void (*fill_shadow)(QLDraw2D *self, QLRect inner, ql_coord_t radius, ql_coord_t blur,
                        ql_coord_t dx, ql_coord_t dy, ql_color_t color, uint8_t alpha);

    /*
     * 直线：Bresenham，thickness > 1 时按方形笔刷加粗（与 ql_canvas_draw_line 同式）。
     * 两个后端都应指向公共实现 ql_draw2d_draw_line()，这样细线不会因后端差像素。
     */
    void (*draw_line)(QLDraw2D *self, ql_coord_t x1, ql_coord_t y1, ql_coord_t x2,
                      ql_coord_t y2, ql_coord_t thickness, ql_color_t color);

    void (*draw_text)(QLDraw2D *self, const QLFont *font, const char *text, int32_t len,
                      QLRect box, ql_color_t color, QLTextAlign align);

    /* 后端可选的批次提交（GPU 用；CPU 为 NULL） */
    void (*flush)(QLDraw2D *self);

    /*
     * CSS px → 设备像素的缩放（26.6 定点，QL_SCALE_ONE = 1.0），默认 QL_SCALE_ONE。
     *
     * 上层的所有坐标（qlui / 控件 / 自定义绘制）**一律是 CSS px**，
     * 只有后端在入口乘这个系数，所以 DPR 变化不需要动任何上层代码。
     * 由 ql_draw2d_canvas_init / ql_gfx_gl_draw 从"表面"继承，也可手动设。
     */
    ql_coord_t scale_fx;

    /*
     * 绘制期平移（CSS px，26.6 定点，64 = 1 CSS px）。默认 0。
     *
     * 这是动画的落点：`translate(0.5px)` 就是把它设成 32。
     * CPU 后端会把矩形转成带小数的 `QLRectFx` 走**面积覆盖率抗锯齿**，
     * GPU 后端直接加进顶点（顶点本来就是浮点，零成本）。
     * **不影响布局**（与 CSS 的 transform 语义一致），所以滑动/缩放动画零重排。
     *
     * 文本与阴影为了保持清晰/性能，只取它的整数部分。
     */
    ql_coord_t offset_x_fx;
    ql_coord_t offset_y_fx;

    void *user;   /* 后端私有数据 */
};

/* 单行绘制文字的公共实现：后端可直接复用它的排版逻辑 */
typedef void (*ql_draw2d_glyph_fn)(QLDraw2D *self, const QLFont *font,
                                   const QLGlyph *glyph, ql_coord_t pen_x,
                                   ql_coord_t baseline_y, ql_color_t color);
void ql_draw2d_text_layout(QLDraw2D *self, const QLFont *font, const char *text, int32_t len,
                           QLRect box, ql_color_t color, QLTextAlign align,
                           ql_draw2d_glyph_fn emit_glyph);

/*
 * 直线的**公共实现**：CPU 与 GPU 后端都应把 draw_line 指向它。
 * 只用 fill_rect 组合，因此两个后端的几何天然一致（不会因为各自的光栅化差像素）。
 */
void ql_draw2d_draw_line(QLDraw2D *self, ql_coord_t x1, ql_coord_t y1, ql_coord_t x2,
                         ql_coord_t y2, ql_coord_t thickness, ql_color_t color);

/* ----- CPU 后端（包一层 QLCanvas） ----- */
ql_err_t ql_draw2d_canvas_init(QLDraw2D *out, QLCanvas *canvas);

/*
 * 按缩放取"设备像素字号"的同族字体：DPR>1 时字形必须按设备字号重新光栅化，
 * 否则会被拉伸变糊。只有动态字体能真正重取；位图字体原样返回。
 */
const QLFont *ql_draw2d_device_font(const QLFont *font, ql_coord_t scale_fx);

/* 绘制期平移（CSS px，26.6 定点）。下一帧的绘制整体偏移这么多 */
static inline void ql_draw2d_set_offset(QLDraw2D *d2d, ql_coord_t x_fx, ql_coord_t y_fx)
{
    if (d2d != NULL) {
        d2d->offset_x_fx = x_fx;
        d2d->offset_y_fx = y_fx;
    }
}

/* 直接改缩放（26.6 定点）。后端实现只在入口乘它，不缓存换算后的坐标 */
static inline void ql_draw2d_set_scale(QLDraw2D *d2d, ql_coord_t scale_fx)
{
    if (d2d != NULL)
        d2d->scale_fx = (scale_fx > 0) ? scale_fx : QL_SCALE_ONE;
}

#endif /* QLGL_DRAW2D_H */
