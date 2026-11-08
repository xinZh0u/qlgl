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
    /* 圆遮罩（CSS px）。NULL = 该后端不支持，上层据此跳过该特性 */
    void (*set_mask)(QLDraw2D *self, ql_coord_t cx, ql_coord_t cy, ql_coord_t r);

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

    /*
     * 毛玻璃（CSS px）：把 `rect` 区域里**已经画好的内容**就地模糊。
     * NULL = 该后端不支持（上层跳过该效果并告警一次）。
     * `sigma` 是高斯标准差（CSS `blur()` 的语义），后端自行换算成盒式半径。
     * 必须在"画元素自己的背景之前"调用，否则会连元素自己一起模糊。
     */
    void (*backdrop_blur)(QLDraw2D *self, QLRect rect, int sigma);

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

    /*
     * 圆遮罩（CSS px 的圆心 + 半径；**r < 0 表示关闭**，r == 0 = 全裁）。
     * CPU 后端换算成设备像素交给 QLCanvas；GPU 后端交给顶点/片元着色器。
     * 它是"裁剪"的一部分：调用方负责在子树画完后恢复。
     */
    ql_coord_t mask_cx;
    ql_coord_t mask_cy;
    ql_coord_t mask_r;      /* < 0 = 关闭 */

    /*
     * 元素缩放（`transform: scale()`，26.6 定点，QL_SCALE_ONE = 1.0）+ 缩放中心。
     * 与 DPR 的 scale_fx 是两回事：那个是设备像素比，这个是"元素自己放大/缩小"。
     * 只支持**等比**：非等比缩放会让圆角、描边、字形的语义变得没有定义。
     */
    ql_coord_t elem_x_fx;   /* 水平缩放（26.6） */
    ql_coord_t elem_y_fx;   /* 垂直缩放（26.6）；与 x 不同 = 非等比（假翻转） */
    ql_coord_t elem_px;     /* 缩放中心（CSS px） */
    ql_coord_t elem_py;

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
/*
 * 圆遮罩；**r < 0 关闭**（r == 0 = 全裁）。单位与其它绘制入口一致：CSS px。
 *
 * 走 vtable（像 set_clip 一样）而不是纯内联：CPU 后端要把它换算成设备像素
 * 推到 QLCanvas 的像素汇点上，GPU 后端要写进顶点属性。
 */
/* 毛玻璃。后端不支持时返回 0（调用方据此跳过自身背景的半透明处理） */
static inline int ql_draw2d_backdrop_blur(QLDraw2D *d2d, QLRect rect, int sigma)
{
    if (d2d == NULL || d2d->backdrop_blur == NULL || sigma <= 0)
        return 0;
    if (ql_rect_is_empty(rect))
        return 0;
    d2d->backdrop_blur(d2d, rect, sigma);
    return 1;
}

static inline void ql_draw2d_set_mask(QLDraw2D *d2d, ql_coord_t cx, ql_coord_t cy,
                                      ql_coord_t r)
{
    if (d2d == NULL)
        return;
    d2d->mask_cx = cx;
    d2d->mask_cy = cy;
    d2d->mask_r = (r < 0) ? -1 : r;
    if (d2d->set_mask != NULL)
        d2d->set_mask(d2d, cx, cy, d2d->mask_r);
}

static inline void ql_draw2d_set_offset(QLDraw2D *d2d, ql_coord_t x_fx, ql_coord_t y_fx)
{
    if (d2d != NULL) {
        d2d->offset_x_fx = x_fx;
        d2d->offset_y_fx = y_fx;
    }
}

/*
 * 元素缩放（`transform: scale()`）：绕 (px, py) 等比缩放后续的所有绘制。
 * CSS px 的"长度"（半径、描边宽度、偏移）也一起缩放，与浏览器一致。
 */
static inline void ql_draw2d_set_elem_scale(QLDraw2D *d2d, ql_coord_t fx,
                                            ql_coord_t px, ql_coord_t py)
{
    if (d2d == NULL)
        return;
    d2d->elem_x_fx = (fx > 0) ? fx : QL_SCALE_ONE;
    d2d->elem_y_fx = d2d->elem_x_fx;
    d2d->elem_px = px;
    d2d->elem_py = py;
}

/* 非等比：水平/垂直分开给。`backface-visibility` 的假翻转就靠这个 */
static inline void ql_draw2d_set_elem_scale_xy(QLDraw2D *d2d, ql_coord_t fx,
                                               ql_coord_t fy, ql_coord_t px,
                                               ql_coord_t py)
{
    if (d2d == NULL)
        return;
    /* 0 是合法值：翻转动画的中间帧就是把元素压到零宽 */
    d2d->elem_x_fx = (fx >= 0) ? fx : QL_SCALE_ONE;
    d2d->elem_y_fx = (fy >= 0) ? fy : QL_SCALE_ONE;
    d2d->elem_px = px;
    d2d->elem_py = py;
}

/* 直接改缩放（26.6 定点）。后端实现只在入口乘它，不缓存换算后的坐标 */
static inline void ql_draw2d_set_scale(QLDraw2D *d2d, ql_coord_t scale_fx)
{
    if (d2d != NULL)
        d2d->scale_fx = (scale_fx > 0) ? scale_fx : QL_SCALE_ONE;
}

#endif /* QLGL_DRAW2D_H */
