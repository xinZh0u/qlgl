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

    void (*draw_text)(QLDraw2D *self, const QLFont *font, const char *text, int32_t len,
                      QLRect box, ql_color_t color, QLTextAlign align);

    /* 后端可选的批次提交（GPU 用；CPU 为 NULL） */
    void (*flush)(QLDraw2D *self);

    void *user;   /* 后端私有数据 */
};

/* 单行绘制文字的公共实现：后端可直接复用它的排版逻辑 */
typedef void (*ql_draw2d_glyph_fn)(QLDraw2D *self, const QLFont *font,
                                   const QLGlyph *glyph, ql_coord_t pen_x,
                                   ql_coord_t baseline_y, ql_color_t color);
void ql_draw2d_text_layout(QLDraw2D *self, const QLFont *font, const char *text, int32_t len,
                           QLRect box, ql_color_t color, QLTextAlign align,
                           ql_draw2d_glyph_fn emit_glyph);

/* ----- CPU 后端（包一层 QLCanvas） ----- */
ql_err_t ql_draw2d_canvas_init(QLDraw2D *out, QLCanvas *canvas);

#endif /* QLGL_DRAW2D_H */
