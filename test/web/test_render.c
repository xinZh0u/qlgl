/**
 * @file    test_render.c
 * @brief   绘制包围盒与脏区重绘（paint_box / ql_ui_render_rect）。
 *
 * 两条契约：
 *   1. `paint_box` 是"子树可能画到的范围"的**上界**——阴影外扩、未裁剪子树都要算进去，
 *      算小了会漏画；
 *   2. 脏区重绘的结果必须与全量重绘**在脏区内逐像素相同**，且脏区外一个像素都不碰。
 *
 * 第 2 条是脏区剪枝唯一有意义的正确性判据：剪枝只要错一次，表现就是"局部刷新后
 * 屏幕上少一块"这种最难查的 bug。
 */
#include <stdio.h>
#include <string.h>

#include "../ql_test.h"

#include "ui_flex_paint.h"

#define W 240
#define H 180

static QLDispDriver *s_full;
static QLDispDriver *s_dirty;
static QLCanvas s_cf;
static QLCanvas s_cd;

static const ql_color_t BG = QL_COLOR_RGB(0x10, 0x18, 0x20);

static int point_in(QLRect r, ql_coord_t x, ql_coord_t y)
{
    return x >= r.x && x < ql_rect_right(r) && y >= r.y && y < ql_rect_bottom(r);
}

/* 按基础样式里的特征找节点（比按尺寸找稳，不受 box-sizing 影响） */
static QLUINodeRT *node_with_radius(QLUIRoot *ui, ql_coord_t radius)
{
    for (uint16_t i = 0; i < ui->node_count; i++) {
        const QLUIStyle *st = ui->nodes[i].style;
        if (st != NULL && st->radius[0] == radius)
            return &ui->nodes[i];
    }
    return NULL;
}

static QLUINodeRT *node_with_overflow(QLUIRoot *ui)
{
    for (uint16_t i = 0; i < ui->node_count; i++) {
        const QLUIStyle *st = ui->nodes[i].style;
        if (st != NULL && st->overflow != QL_UI_OVERFLOW_VISIBLE)
            return &ui->nodes[i];
    }
    return NULL;
}

/*
 * 脏区重绘 vs 全量重绘。
 * 返回 0 = 通过；否则返回出错像素数（并打印第一个不一致的位置）。
 */
static int compare_dirty(QLUIRoot *ui, QLRect dirty, const char *what)
{
    /* 全量：整幅画进 s_full */
    ql_canvas_from_disp(&s_cf, s_full);
    ql_canvas_clear(&s_cf, BG);
    ql_ui_render_canvas(ui, &s_cf);

    /* 脏区：先把整幅刷成背景色，再只重绘 dirty */
    ql_canvas_from_disp(&s_cd, s_dirty);
    ql_canvas_clear(&s_cd, BG);
    QLDraw2D d2d;
    if (ql_draw2d_canvas_init(&d2d, &s_cd) != QL_OK)
        return -1;
    ui->root_clip_valid = 0;      /* 让 render_rect 从视口算起，别继承上一轮的裁剪 */
    ql_ui_render_rect(ui, &d2d, dirty);

    int bad = 0;
    for (ql_coord_t y = 0; y < H; y++) {
        for (ql_coord_t x = 0; x < W; x++) {
            ql_color_t got = ql_disp_mem_pixel(s_dirty, x, y);
            ql_color_t want = point_in(dirty, x, y) ? ql_disp_mem_pixel(s_full, x, y) : BG;
            if (got != want) {
                if (bad == 0) {
                    printf("  [%s] 首个不一致 (%d,%d)：脏区内应 %06X，实得 %06X\n",
                           what, (int)x, (int)y, (unsigned)want, (unsigned)got);
                }
                bad++;
            }
        }
    }
    return bad;
}

QL_TEST_BEGIN("test_render")

    s_full = ql_disp_mem_create(W, H, QL_COLOR_ARGB8888);
    s_dirty = ql_disp_mem_create(W, H, QL_COLOR_ARGB8888);

    QLUIRoot *ui = ql_ui_build(&ui_flex_paint_doc);
    QL_CHECK(ui != NULL);
    if (ui == NULL) {
        ql_disp_mem_destroy(s_full);
        ql_disp_mem_destroy(s_dirty);
        printf("  %d 项检查，%d 项失败\n", ql_test_checks, ql_test_failures);
        return 1;
    }

    ql_ui_layout(ui, W, H);

    QLUINodeRT *card = node_with_radius(ui, 6);     /* .card */
    QLUINodeRT *clip = node_with_overflow(ui);      /* .clip */
    QLUINodeRT *big = (clip != NULL) ? clip->first_child : NULL;   /* .big */
    QL_CHECK(card != NULL);
    QL_CHECK(clip != NULL);
    QL_CHECK(big != NULL);
    if (card == NULL || clip == NULL || big == NULL) {
        ql_ui_free(ui);
        ql_disp_mem_destroy(s_full);
        ql_disp_mem_destroy(s_dirty);
        printf("  %d 项检查，%d 项失败\n", ql_test_checks, ql_test_failures);
        return 1;
    }

    const QLRect cr = card->border_box;
    const QLRect kr = clip->border_box;
    const QLRect bounds = ql_ui_paint_bounds(ui);

    /* ---------- 1. paint_box 必须包含阴影外扩区 ---------- */
    /* box-shadow: 0 4px 10px → 外扩 blur+1 = 11、下移 4 */
    QL_CHECK(point_in(bounds, cr.x + cr.w / 2, ql_rect_bottom(cr) + 8));   /* 卡下方光晕 */
    QL_CHECK(point_in(bounds, cr.x + 4, ql_rect_bottom(cr) + 8));          /* 圆角外沿 */
    QL_CHECK(!point_in(bounds, 4, H + 30));   /* 谁都不在那儿 */

    /* ---------- 2. overflow:hidden 的容器：包围盒不外扩 ---------- */
    /* 子节点远大于容器，但被裁到容器的边框盒 */
    QL_CHECK(ql_rect_is_empty(ql_rect_intersect(
        ql_rect_make(ql_rect_right(kr), kr.y, 8, kr.h), clip->paint_box)) == 1);
    QL_CHECK_EQ(clip->paint_box.x, kr.x);
    QL_CHECK_EQ(clip->paint_box.y, kr.y);
    QL_CHECK_EQ(clip->paint_box.w, kr.w);
    QL_CHECK_EQ(clip->paint_box.h, kr.h);

    /* 被裁掉的子节点自身的 paint_box 仍是它自己的范围（不能是陈旧值） */
    QL_CHECK_EQ(big->paint_box.w, big->border_box.w);

    /* ---------- 3. 绝对定位 + 阴影：包围盒允许超出视口 ---------- */
    /* .badge 贴底 8px，自身 22 高 + 阴影外扩 9 → 底边超过视口 */
    QL_CHECK(ql_rect_bottom(bounds) > H);
    QL_CHECK(ql_rect_right(bounds) > W - 8);

    /* ---------- 4. 脏区重绘：与全量重绘逐像素一致 ---------- */
    {
        const QLRect rects[] = {
            { 0, 0, W, H },                              /* 整幅 */
            { cr.x, cr.y, cr.w, cr.h },                  /* 只覆盖卡片本体 */
            { cr.x, ql_rect_bottom(cr) + 2, 40, 10 },    /* 只有阴影光晕，不含本体 */
            { kr.x - 6, kr.y - 6, 30, 20 },              /* 裁剪容器的左上角 */
            { kr.x + 40, kr.y + 10, 60, 40 },            /* 跨出裁剪容器右边 */
            { 170, 130, 60, 50 },                        /* 绝对定位 badge */
            { 0, 0, 6, 6 },                              /* 角落小片 */
            { 300, 300, 40, 40 },                        /* 完全在视口外 */
            { 20, 95, 10, 10 },                          /* 卡片与容器之间的空白 */
        };
        const char *names[] = { "整幅", "卡片本体", "仅光晕", "裁剪容器", "跨裁剪边界",
                                "badge", "角落", "视口外", "空白区" };

        for (int i = 0; i < (int)(sizeof(rects) / sizeof(rects[0])); i++)
            QL_CHECK_EQ(compare_dirty(ui, rects[i], names[i]), 0);
    }

    /* ---------- 5. 布局变化后重新计算，不残留旧包围盒 ---------- */
    {
        ql_ui_layout(ui, W, H);
        QLRect again = ql_ui_paint_bounds(ui);
        QL_CHECK_EQ(again.x, bounds.x);
        QL_CHECK_EQ(again.y, bounds.y);
        QL_CHECK_EQ(again.w, bounds.w);
        QL_CHECK_EQ(again.h, bounds.h);
        QL_CHECK_EQ(compare_dirty(ui, ql_rect_make(0, 0, W, H), "二次布局"), 0);
    }

    /* ---------- 6. 视口变窄：fixed/absolute 跟着走，包围盒同步收缩 ---------- */
    {
        ql_ui_layout(ui, 160, H);
        QLRect narrow = ql_ui_paint_bounds(ui);
        QL_CHECK(narrow.w < bounds.w);    /* 视口变窄，包围盒跟着收 */
        QL_CHECK_EQ(compare_dirty(ui, ql_rect_make(0, 0, W, H), "窄视口"), 0);
    }

    ql_ui_free(ui);
    ql_disp_mem_destroy(s_full);
    ql_disp_mem_destroy(s_dirty);
    s_full = NULL;
    s_dirty = NULL;

QL_TEST_END()
