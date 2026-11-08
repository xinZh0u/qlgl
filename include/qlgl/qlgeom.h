/**
 * @file    qlgeom.h
 * @brief   几何与颜色基础类型。所有坐标固定 32 位，保证 32/64 位行为一致。
 */
#ifndef QLGL_GEOM_H
#define QLGL_GEOM_H

#include "qlgl.h"

/* 坐标类型：可整体改成 int16_t 以节省 MCU 内存，接口保持不变 */
typedef int32_t ql_coord_t;

/* 颜色：0xRRGGBB，与具体像素格式无关；落屏时由画布负责转换 */
typedef uint32_t ql_color_t;

#define QL_COLOR_RGB(r, g, b)                                                \
    ((((uint32_t)(r) & 0xFFu) << 16) | (((uint32_t)(g) & 0xFFu) << 8) |      \
     ((uint32_t)(b) & 0xFFu))

#define QL_COLOR_BLACK   QL_COLOR_RGB(0x00, 0x00, 0x00)
#define QL_COLOR_WHITE   QL_COLOR_RGB(0xFF, 0xFF, 0xFF)
#define QL_COLOR_RED     QL_COLOR_RGB(0xFF, 0x00, 0x00)
#define QL_COLOR_GREEN   QL_COLOR_RGB(0x00, 0xFF, 0x00)
#define QL_COLOR_BLUE    QL_COLOR_RGB(0x00, 0x00, 0xFF)
#define QL_COLOR_GRAY    QL_COLOR_RGB(0x80, 0x80, 0x80)
#define QL_COLOR_SKY     QL_COLOR_RGB(0x33, 0x99, 0xFF)

/* 提色分量 */
#define QL_COLOR_GET_R(c)   (uint8_t)(((c) >> 16) & 0xFFu)
#define QL_COLOR_GET_G(c)   (uint8_t)(((c) >> 8) & 0xFFu)
#define QL_COLOR_GET_B(c)   (uint8_t)((c) & 0xFFu)

/* 矩形：x/y 为左上角，w/h 为尺寸（不含右/下边界） */
typedef struct {
    ql_coord_t x;
    ql_coord_t y;
    ql_coord_t w;
    ql_coord_t h;
} QLRect;

static inline QLRect ql_rect_make(ql_coord_t x, ql_coord_t y, ql_coord_t w, ql_coord_t h)
{
    QLRect r = { x, y, w, h };
    return r;
}

static inline int ql_rect_is_empty(QLRect r)
{
    return r.w <= 0 || r.h <= 0;
}

static inline ql_coord_t ql_rect_right(QLRect r)  { return r.x + r.w; }
static inline ql_coord_t ql_rect_bottom(QLRect r) { return r.y + r.h; }

static inline QLRect ql_rect_intersect(QLRect a, QLRect b)
{
    ql_coord_t x1 = a.x > b.x ? a.x : b.x;
    ql_coord_t y1 = a.y > b.y ? a.y : b.y;
    ql_coord_t x2 = ql_rect_right(a) < ql_rect_right(b) ? ql_rect_right(a) : ql_rect_right(b);
    ql_coord_t y2 = ql_rect_bottom(a) < ql_rect_bottom(b) ? ql_rect_bottom(a) : ql_rect_bottom(b);
    if (x2 <= x1 || y2 <= y1)
        return ql_rect_make(0, 0, 0, 0);
    return ql_rect_make(x1, y1, x2 - x1, y2 - y1);
}

static inline QLRect ql_rect_union(QLRect a, QLRect b)
{
    if (ql_rect_is_empty(a))
        return b;
    if (ql_rect_is_empty(b))
        return a;
    ql_coord_t x1 = a.x < b.x ? a.x : b.x;
    ql_coord_t y1 = a.y < b.y ? a.y : b.y;
    ql_coord_t x2 = ql_rect_right(a) > ql_rect_right(b) ? ql_rect_right(a) : ql_rect_right(b);
    ql_coord_t y2 = ql_rect_bottom(a) > ql_rect_bottom(b) ? ql_rect_bottom(a) : ql_rect_bottom(b);
    return ql_rect_make(x1, y1, x2 - x1, y2 - y1);
}

static inline int ql_rect_overlaps(QLRect a, QLRect b)
{
    return !ql_rect_is_empty(ql_rect_intersect(a, b));
}

/* ------------------------- CSS px ↔ 设备像素（DPR） ------------------------- */

/*
 * 缩放系数用 26.6 定点表示：QL_SCALE_ONE = 1.0，96 = 1.5，192 = 3.0。
 * 布局一律在 CSS px 里做；只有后端的绘制入口才乘这个系数。
 */
#define QL_SCALE_SHIFT 6
#define QL_SCALE_ONE   (1 << QL_SCALE_SHIFT)

/* 长度换算：向下取整（与"像素对齐到左上"一致） */
static inline ql_coord_t ql_scale_len(ql_coord_t v, ql_coord_t scale_fx)
{
    return (ql_coord_t)(((int64_t)v * (int64_t)scale_fx) >> QL_SCALE_SHIFT);
}

/*
 * 矩形换算必须**按边**缩放，不能 x*scale + w*scale：
 * 相邻矩形各自算宽度会因为分别取整而在接缝处裂开 1px 或重叠 1px。
 * 用同一条边界值（ql_rect_right / ql_rect_bottom）算，天然严丝合缝。
 */
static inline QLRect ql_scale_rect(QLRect r, ql_coord_t scale_fx)
{
    ql_coord_t x1 = ql_scale_len(r.x, scale_fx);
    ql_coord_t y1 = ql_scale_len(r.y, scale_fx);
    ql_coord_t x2 = ql_scale_len(ql_rect_right(r), scale_fx);
    ql_coord_t y2 = ql_scale_len(ql_rect_bottom(r), scale_fx);
    return ql_rect_make(x1, y1, x2 - x1, y2 - y1);
}

/* 设备像素 → CSS px：近边向下、远边向上取整，保证覆盖所有设备像素 */
static inline ql_coord_t ql_unscale_len(ql_coord_t v, ql_coord_t scale_fx)
{
    if (scale_fx <= 0)
        return v;
    return (ql_coord_t)(((int64_t)v * (int64_t)QL_SCALE_ONE) / (int64_t)scale_fx);
}

static inline ql_coord_t ql_unscale_len_ceil(ql_coord_t v, ql_coord_t scale_fx)
{
    if (scale_fx <= 0)
        return v;
    int64_t num = (int64_t)v * (int64_t)QL_SCALE_ONE;
    int64_t q = num / (int64_t)scale_fx;
    if (num % (int64_t)scale_fx != 0)
        q++;
    return (ql_coord_t)q;
}

static inline QLRect ql_unscale_rect(QLRect r, ql_coord_t scale_fx)
{
    ql_coord_t x1 = ql_unscale_len(r.x, scale_fx);
    ql_coord_t y1 = ql_unscale_len(r.y, scale_fx);
    ql_coord_t x2 = ql_unscale_len_ceil(ql_rect_right(r), scale_fx);
    ql_coord_t y2 = ql_unscale_len_ceil(ql_rect_bottom(r), scale_fx);
    return ql_rect_make(x1, y1, x2 - x1, y2 - y1);
}

/* 点是否落在圆角矩形内（radius 会被裁到短边的一半） */
int ql_rect_contains_round(QLRect r, ql_coord_t px, ql_coord_t py, ql_coord_t radius);

#endif /* QLGL_GEOM_H */
