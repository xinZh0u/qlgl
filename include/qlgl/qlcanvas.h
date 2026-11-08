/**
 * @file    qlcanvas.h
 * @brief   画布：一组带裁剪的绘制原语，直接写入显示驱动的帧缓冲。
 *
 * 坐标一律是"屏幕绝对坐标"，裁剪由 canvas->clip 负责。
 * v0.1 不提供图层合成，只支持"按 alpha 与已有像素混合"。
 */
#ifndef QLGL_CANVAS_H
#define QLGL_CANVAS_H

#include "qlgl.h"
#include "qlgeom.h"
#include "qldisp.h"
#include "qlfont.h"

typedef struct {
    uint8_t  *buffer;    /* 帧缓冲起点 */
    uint32_t  stride;    /* 字节/行 */
    ql_coord_t width;    /* 缓冲宽度（像素） */
    ql_coord_t height;   /* 缓冲高度（像素） */
    QLRect    clip;      /* 裁剪区，屏幕坐标 */
    uint8_t   format;    /* QLColorFormat */
} QLCanvas;

/* 由显示驱动构造画布，clip 默认整屏 */
ql_err_t ql_canvas_from_disp(QLCanvas *canvas, QLDispDriver *driver);
ql_err_t ql_canvas_from_disp_clip(QLCanvas *canvas, QLDispDriver *driver, QLRect clip);

void ql_canvas_set_clip(QLCanvas *canvas, QLRect clip);
QLRect ql_canvas_clip(QLCanvas *canvas);

/* 单像素读写（屏幕坐标，自动按 format 转换） */
ql_color_t ql_canvas_read_pixel(QLCanvas *canvas, ql_coord_t x, ql_coord_t y);
void       ql_canvas_write_pixel(QLCanvas *canvas, ql_coord_t x, ql_coord_t y,
                                 ql_color_t color, uint8_t alpha);

/* ----- 原语 ----- */
void ql_canvas_clear(QLCanvas *canvas, ql_color_t color);
void ql_canvas_fill_rect(QLCanvas *canvas, QLRect rect, ql_color_t color);
/* alpha: 0 完全透明（跳过），255 完全不透明 */
void ql_canvas_fill_rect_alpha(QLCanvas *canvas, QLRect rect, ql_color_t color, uint8_t alpha);
void ql_canvas_fill_round_rect(QLCanvas *canvas, QLRect rect, ql_coord_t radius,
                               ql_color_t color);
void ql_canvas_fill_round_rect_alpha(QLCanvas *canvas, QLRect rect, ql_coord_t radius,
                                     ql_color_t color, uint8_t alpha);
/* 线性渐变方向 */
typedef enum {
    QL_GRADIENT_VERTICAL = 0,     /* 从 rect 上边到下边 */
    QL_GRADIENT_HORIZONTAL,       /* 从 rect 左边到右边 */
} QLGradientDir;

/*
 * 线性渐变圆角矩形（单趟，不需要离屏缓冲）。
 * 垂直方向是快路径：每行颜色相同，直接走行填充，成本≈纯色填充。
 * 水平方向每像素颜色不同，逐像素混合，比垂直慢一个量级。
 */
void ql_canvas_fill_round_rect_gradient(QLCanvas *canvas, QLRect rect, ql_coord_t radius,
                                        ql_color_t from, ql_color_t to,
                                        QLGradientDir dir, uint8_t alpha);

/*
 * 软阴影：以 inner 矩形（圆角 radius）为轮廓，向外扩散 blur 像素，
 * 整体偏移 (dx, dy)，用 SDF + smoothstep 衰减（与 GPU 后端同一套公式）。
 *
 * 实现要点：只有"环"（距轮廓 blur 以内）需要逐像素算距离，
 * 内部实心区走 fill_row，因此在 PC 上不明显，在 MCU 上也可用。
 */
void ql_canvas_fill_shadow(QLCanvas *canvas, QLRect inner, ql_coord_t radius,
                           ql_coord_t blur, ql_coord_t dx, ql_coord_t dy,
                           ql_color_t color, uint8_t alpha);

/* 描边：宽度向外不扩张，向内绘制 */
void ql_canvas_stroke_rect(QLCanvas *canvas, QLRect rect, ql_coord_t radius,
                           ql_coord_t thickness, ql_color_t color);
void ql_canvas_draw_hline(QLCanvas *canvas, ql_coord_t x1, ql_coord_t x2, ql_coord_t y,
                          ql_coord_t thickness, ql_color_t color);
void ql_canvas_draw_vline(QLCanvas *canvas, ql_coord_t x, ql_coord_t y1, ql_coord_t y2,
                          ql_coord_t thickness, ql_color_t color);
void ql_canvas_draw_line(QLCanvas *canvas, ql_coord_t x1, ql_coord_t y1,
                         ql_coord_t x2, ql_coord_t y2, ql_coord_t thickness,
                         ql_color_t color);

/* ----- 文本 ----- */
typedef enum {
    QL_TEXT_ALIGN_LEFT = 0,
    QL_TEXT_ALIGN_CENTER,
    QL_TEXT_ALIGN_RIGHT,
} QLTextAlign;

/*
 * 在 box 内绘制文本：水平按 align 对齐，垂直从 box.y 开始逐行排。
 * 支持 '\n' 换行；不自动折行。len < 0 表示以 '\0' 结尾。
 * 超出 box 或 canvas 裁剪区的像素自动丢弃。
 */
void ql_canvas_draw_text(QLCanvas *canvas, const QLFont *font, const char *text,
                         int32_t len, QLRect box, ql_color_t color, QLTextAlign align);

/* 单个字形绘制（高级用法；坐标是基线笔位） */
void ql_canvas_draw_glyph(QLCanvas *canvas, const QLFont *font, const QLGlyph *glyph,
                          ql_coord_t pen_x, ql_coord_t baseline_y, ql_color_t color);

/* 颜色按目标格式量化后的实际值（测试用：RGB565 会有精度损失） */
ql_color_t ql_canvas_quantize(QLCanvas *canvas, ql_color_t color);

#endif /* QLGL_CANVAS_H */
