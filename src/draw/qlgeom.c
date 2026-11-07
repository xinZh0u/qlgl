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

    ql_coord_t ix1 = r.x + radius;
    ql_coord_t iy1 = r.y + radius;
    ql_coord_t ix2 = ql_rect_right(r) - 1 - radius;
    ql_coord_t iy2 = ql_rect_bottom(r) - 1 - radius;

    ql_coord_t dx = 0;
    ql_coord_t dy = 0;
    if (px < ix1)
        dx = ix1 - px;
    else if (px > ix2)
        dx = px - ix2;
    if (py < iy1)
        dy = iy1 - py;
    else if (py > iy2)
        dy = py - iy2;

    return dx * dx + dy * dy <= radius * radius;
}
