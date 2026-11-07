/**
 * @file    qlrender.c
 * @brief   脏矩形管理与对象树遍历绘制。
 */
#include <string.h>

#include <qlgl/qlgl.h>

static QLObject *s_root;
static QLRect    s_dirty[QL_CFG_DIRTY_RECT_MAX];
static uint32_t  s_dirty_count;
static uint8_t   s_all_dirty = 1;

static void render_tree_hook(QLObject *obj)
{
    ql_render_invalidate(obj);
}

ql_err_t ql_render_set_root(QLObject *root)
{
    if (root == NULL)
        return QL_ERR_INVAL;
    s_root = root;
    s_all_dirty = 1;
    ql_object_set_tree_hook(render_tree_hook);
    return QL_OK;
}

QLObject *ql_render_root(void)
{
    return s_root;
}

void ql_render_invalidate_all(void)
{
    s_all_dirty = 1;
}

/* 两矩形相交就直接合并，最多合并到只剩一个 */
static void dirty_add(QLRect area, QLRect screen)
{
    area = ql_rect_intersect(area, screen);
    if (ql_rect_is_empty(area))
        return;

    for (uint32_t i = 0; i < s_dirty_count; i++) {
        if (ql_rect_overlaps(s_dirty[i], area)) {
            s_dirty[i] = ql_rect_union(s_dirty[i], area);
            /* 合并后可能与其他矩形也相交，做一次收敛 */
            for (uint32_t j = 0; j < s_dirty_count; j++) {
                if (j == i || !ql_rect_overlaps(s_dirty[i], s_dirty[j]))
                    continue;
                s_dirty[i] = ql_rect_union(s_dirty[i], s_dirty[j]);
                s_dirty[j] = s_dirty[s_dirty_count - 1];
                s_dirty_count--;
                j = (uint32_t)-1;   /* 重新扫描 */
            }
            return;
        }
    }

    if (s_dirty_count < QL_CFG_DIRTY_RECT_MAX) {
        s_dirty[s_dirty_count++] = area;
        return;
    }

    /* 满了：全部并成一块，保证正确性优先 */
    QLRect merged = area;
    for (uint32_t i = 0; i < s_dirty_count; i++)
        merged = ql_rect_union(merged, s_dirty[i]);
    s_dirty[0] = merged;
    s_dirty_count = 1;
}

void ql_render_invalidate_rect(QLRect area)
{
    QLDispDriver *disp = ql_disp_get();
    if (disp != NULL)
        dirty_add(area, ql_rect_make(0, 0, disp->width, disp->height));
    else
        s_all_dirty = 1;
}

void ql_render_invalidate(QLObject *obj)
{
    if (obj == NULL || !ql_object_is(obj, QL_WIDGET_TYPE)) {
        s_all_dirty = 1;
        return;
    }
    /* 还没挂进渲染树的对象不在屏幕上，不必打脏 */
    if (s_root != NULL && obj != s_root && ql_object_parent(obj) == NULL)
        return;
    ql_render_invalidate_rect(ql_widget_abs_rect(obj));
}

uint32_t ql_render_dirty_count(void)
{
    if (s_all_dirty)
        return 1;
    return s_dirty_count;
}

void ql_render_dirty_rect(uint32_t index, QLRect *out)
{
    if (out == NULL)
        return;
    if (s_all_dirty) {
        QLDispDriver *disp = ql_disp_get();
        *out = (disp != NULL) ? ql_rect_make(0, 0, disp->width, disp->height)
                              : ql_rect_make(0, 0, 0, 0);
        return;
    }
    if (index < s_dirty_count)
        *out = s_dirty[index];
    else
        *out = ql_rect_make(0, 0, 0, 0);
}

/* ------------------------------ 遍历绘制 ------------------------------ */

static void render_node(QLObject *node, ql_coord_t ox, ql_coord_t oy,
                        QLRect clip, QLCanvas *canvas)
{
    if (node == NULL)
        return;
    if ((node->flags & QL_OBJ_FLAG_HIDDEN) != 0)
        return;

    ql_coord_t child_ox = ox;
    ql_coord_t child_oy = oy;
    QLRect child_clip = clip;

    if (ql_object_is(node, QL_WIDGET_TYPE)) {
        if (!ql_widget_is_visible(node))
            return;

        child_ox = ox + ql_widget_x(node);
        child_oy = oy + ql_widget_y(node);

        QLRect box = ql_rect_make(child_ox, child_oy, ql_widget_width(node),
                                  ql_widget_height(node));
        QLRect self_clip = ql_rect_intersect(clip, box);

        if (!ql_rect_is_empty(self_clip)) {
            ql_widget_draw_func draw = ql_widget_get_draw(node);
            if (draw != NULL) {
                QLCanvas sub = *canvas;
                ql_canvas_set_clip(&sub, self_clip);
                draw((QLWidget *)(void *)node, &sub);
            }
        }

        if (ql_widget_clips_children(node))
            child_clip = ql_rect_intersect(clip, box);
    }

    if (ql_rect_is_empty(child_clip))
        return;

    for (QLObject *child = ql_object_first_child(node); child != NULL;
         child = ql_object_next_sibling(child)) {
        render_node(child, child_ox, child_oy, child_clip, canvas);
    }
}

int ql_render(void)
{
    QLDispDriver *disp = ql_disp_get();
    if (disp == NULL || s_root == NULL)
        return QL_ERR_NOT_INIT;

    QLRect screen = ql_rect_make(0, 0, disp->width, disp->height);

    /* 收集本次要画的区域 */
    QLRect areas[QL_CFG_DIRTY_RECT_MAX];
    uint32_t count;

    if (s_all_dirty) {
        areas[0] = screen;
        count = 1;
    } else {
        count = s_dirty_count;
        for (uint32_t i = 0; i < count; i++)
            areas[i] = s_dirty[i];
    }

    if (count == 0)
        return 0;

    QLCanvas canvas;
    if (ql_canvas_from_disp(&canvas, disp) != QL_OK)
        return QL_ERR_NOT_INIT;

    ql_color_t bg = ql_disp_bg_color();

    for (uint32_t i = 0; i < count; i++) {
        QLRect area = areas[i];
        if (ql_rect_is_empty(area))
            continue;

        /* 1. 背景 */
        QLCanvas clear_canvas = canvas;
        ql_canvas_set_clip(&clear_canvas, area);
        ql_canvas_clear(&clear_canvas, bg);

        /* 2. 控件树（裁剪到脏区） */
        QLCanvas draw_canvas = canvas;
        ql_canvas_set_clip(&draw_canvas, area);
        render_node(s_root, 0, 0, area, &draw_canvas);

        /* 3. 推屏 */
        if (disp->flush != NULL) {
            if (disp->flush(disp, area) != 0)
                QL_LOGW("display flush failed for area %d,%d %dx%d",
                        (int)area.x, (int)area.y, (int)area.w, (int)area.h);
        }
    }

    if (disp->flush_done != NULL)
        disp->flush_done(disp);

    s_dirty_count = 0;
    s_all_dirty = 0;
    return (int)count;
}
