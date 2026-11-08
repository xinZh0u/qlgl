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
    /* 传 NULL 表示解绑（销毁控件树前必须调用，否则脏矩形钩子会打到野指针） */
    s_root = root;
    s_all_dirty = 1;
    ql_object_set_tree_hook(root != NULL ? render_tree_hook : NULL);
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
                        QLRect clip, QLDraw2D *d2d)
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
                /* 控件自己只看到自己的盒子：画到盒子外的内容一律丢弃 */
                d2d->set_clip(d2d, self_clip);
                draw((QLWidget *)(void *)node, d2d);
            }
        }

        if (ql_widget_clips_children(node))
            child_clip = ql_rect_intersect(clip, box);
    }

    if (ql_rect_is_empty(child_clip))
        return;

    d2d->set_clip(d2d, child_clip);
    for (QLObject *child = ql_object_first_child(node); child != NULL;
         child = ql_object_next_sibling(child)) {
        render_node(child, child_ox, child_oy, child_clip, d2d);
    }
}

static uint32_t collect_areas(QLRect *areas, uint32_t cap, ql_coord_t surface_w,
                             ql_coord_t surface_h)
{
    if (s_all_dirty) {
        areas[0] = ql_rect_make(0, 0, surface_w, surface_h);
        return 1;
    }
    uint32_t count = s_dirty_count;
    if (count > cap)
        count = cap;
    for (uint32_t i = 0; i < count; i++)
        areas[i] = s_dirty[i];
    return count;
}

int ql_render_draw2d(QLDraw2D *d2d, ql_coord_t surface_w, ql_coord_t surface_h)
{
    if (d2d == NULL || s_root == NULL || d2d->set_clip == NULL)
        return QL_ERR_INVAL;

    QLRect areas[QL_CFG_DIRTY_RECT_MAX];
    uint32_t count = collect_areas(areas, QL_CFG_DIRTY_RECT_MAX, surface_w, surface_h);
    if (count == 0)
        return 0;

    for (uint32_t i = 0; i < count; i++) {
        QLRect area = areas[i];
        if (ql_rect_is_empty(area))
            continue;
        d2d->set_clip(d2d, area);
        render_node(s_root, 0, 0, area, d2d);
    }
    return (int)count;
}

int ql_render(void)
{
    QLDispDriver *disp = ql_disp_get();
    if (disp == NULL || s_root == NULL)
        return QL_ERR_NOT_INIT;

    /*
     * 脏区队列里全是 CSS px（控件几何就是 CSS px）。清屏要设备像素、
     * 绘制接口吃 CSS px、推屏要设备像素——三种单位在下面各自换算一次。
     */
    QLRect areas[QL_CFG_DIRTY_RECT_MAX];
    uint32_t count = collect_areas(areas, QL_CFG_DIRTY_RECT_MAX, ql_disp_css_width(disp),
                                  ql_disp_css_height(disp));
    if (count == 0)
        return 0;

    QLCanvas canvas;
    if (ql_canvas_from_disp(&canvas, disp) != QL_OK)
        return QL_ERR_NOT_INIT;

    QLDraw2D d2d;
    if (ql_draw2d_canvas_init(&d2d, &canvas) != QL_OK)
        return QL_ERR_NOT_INIT;

    ql_color_t bg = ql_disp_bg_color();
    const ql_coord_t scale_fx = (canvas.scale_fx > 0) ? canvas.scale_fx : QL_SCALE_ONE;

    for (uint32_t i = 0; i < count; i++) {
        QLRect area = areas[i];
        if (ql_rect_is_empty(area))
            continue;

        const QLRect area_dev = (scale_fx == QL_SCALE_ONE)
                                    ? area
                                    : ql_scale_rect(area, scale_fx);

        /* 1. 背景（CPU 后端顺手清掉；GPU 由调用方用 ql_render_draw2d 自己处理） */
        QLCanvas clear_canvas = canvas;
        ql_canvas_set_clip(&clear_canvas, area_dev);
        ql_canvas_clear(&clear_canvas, bg);

        /* 2. 控件树（裁剪到脏区，CSS px） */
        d2d.set_clip(&d2d, area);
        render_node(s_root, 0, 0, area, &d2d);

        /* 3. 推屏（驱动只认设备像素） */
        if (disp->flush != NULL) {
            if (disp->flush(disp, area_dev) != 0)
                QL_LOGW("display flush failed for area %d,%d %dx%d",
                        (int)area_dev.x, (int)area_dev.y, (int)area_dev.w, (int)area_dev.h);
        }
    }

    if (disp->flush_done != NULL)
        disp->flush_done(disp);

    s_dirty_count = 0;
    s_all_dirty = 0;
    return (int)count;
}
