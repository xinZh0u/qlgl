/**
 * @file    qlgeom.c
 * @brief   几何辅助：圆角矩形的点包含测试。
 */
#include <qlgl/qlgl.h>

int ql_rect_contains_round(QLRect r, ql_coord_t px, ql_coord_t py, ql_coord_t radius)
{
    if (px < r.x || px >= ql_rect_right(r) || py < r.y || py >= ql_rect_bottom(r))
        return 0;

    ql_coord_t limit = (r.w < r.h ? r.w : r.h) / 2;
    if (radius <= 0)
        return 1;
    if (radius > limit)
        radius = limit;
    if (radius <= 0)
        return 1;

    const ql_coord_t ix1 = ql_coord_add_sat(r.x, radius);
    const ql_coord_t iy1 = ql_coord_add_sat(r.y, radius);
    const ql_coord_t ix2 = ql_coord_sub_sat(ql_coord_sub_sat(ql_rect_right(r), 1), radius);
    const ql_coord_t iy2 = ql_coord_sub_sat(ql_coord_sub_sat(ql_rect_bottom(r), 1), radius);

    /* 全部扩到 64 位：半径超过 46340 时 dx*dx + dy*dy 会溢出 int32 */
    const int64_t dx = (px < ix1) ? (int64_t)ix1 - px : (px > ix2 ? (int64_t)px - ix2 : 0);
    const int64_t dy = (py < iy1) ? (int64_t)iy1 - py : (py > iy2 ? (int64_t)py - iy2 : 0);
    const int64_t rr = (int64_t)radius * (int64_t)radius;

    return dx * dx + dy * dy <= rr;
}
