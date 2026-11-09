/**
 * @file    test_blur_dirty.c
 * @brief   脏区模式下的毛玻璃：脏矩形必须**完整覆盖**与它相交的玻璃元素。
 *
 * 为什么：毛玻璃读的是帧缓冲里已经画好的像素。脏区只盖住玻璃一角时，
 * 那一角用新背景重新糊、其余部分还是上一帧的糊法 → 交界处出现接缝，
 * 而且模糊核在角的边缘取不到外面的邻居，颜色会断。
 *
 * 做法：下层色块在玻璃底下横向来回跑，每帧只重绘脏区；断言
 *   1. 任何碰到玻璃的脏矩形都完整包含玻璃的绘制盒；
 *   2. 逐帧只重绘脏区得到的画面，与"整屏重绘"逐像素一致（接缝会立刻暴露）；
 *   3. 玻璃自己动的时候同理。
 */
#include "../ql_test.h"

#define W 200
#define H 160

/*
 * 逐帧推进动画、只重绘脏区；返回"脏区未完整覆盖玻璃"的次数。
 *
 * 注意：`ql_draw2d_canvas_init()` 背后是**一份静态上下文**，同一时刻只能有一个
 * canvas 后端的 draw2d 有效。所以这里只做脏区重绘，整屏重绘留到最后一帧
 * 用另一块画布做（见 compare_with_full）。
 */
static int run_dirty(QLUIRoot *ui, QLDraw2D *d2d, int frames)
{
    int bad_cover = 0;
    for (int f = 1; f <= frames; f++) {
        (void)ql_ui_anim_tick(ui, (uint32_t)(f * 80));

        QLRect glass = ql_rect_make(0, 0, 0, 0);
        for (uint16_t i = 0; i < ui->node_count; i++)
            if (ql_ui_node_style(&ui->nodes[i])->backdrop_blur > 0)
                glass = ui->nodes[i].paint_box;

        QLRect areas[16];
        int n = ql_ui_consume_dirty(ui, areas, 16);
        for (int k = 0; k < n; k++) {
            if (!ql_rect_overlaps(areas[k], glass))
                continue;
            const QLRect r = areas[k];
            if (!(r.x <= glass.x && r.y <= glass.y &&
                  ql_rect_right(r) >= ql_rect_right(glass) &&
                  ql_rect_bottom(r) >= ql_rect_bottom(glass)))
                bad_cover++;
            ql_ui_render_rect(ui, d2d, r);
        }
    }
    return bad_cover;
}

/* 把整屏重绘的结果画到另一块画布上，返回与 dirty 画布不同的像素数 */
static long compare_with_full(QLUIRoot *ui, QLDispDriver *dirty_disp, int w, int h)
{
    QLDispDriver *fd = ql_disp_mem_create(w, h, QL_COLOR_RGB888);
    if (fd == NULL)
        return -1;
    QLCanvas fc;
    if (ql_canvas_from_disp(&fc, fd) != QL_OK) {
        ql_disp_mem_destroy(fd);
        return -1;
    }
    ql_canvas_clear(&fc, 0xFFFFFF);
    ql_ui_render_canvas(ui, &fc);

    long diff = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (ql_disp_mem_pixel(dirty_disp, x, y) != ql_disp_mem_pixel(fd, x, y))
                diff++;
    ql_disp_mem_destroy(fd);
    return diff;
}

QL_TEST_BEGIN("test_blur_dirty")

    /*
     * 场景：白色底 + 一块蓝色方块在玻璃底下横向来回跑 + 一块静止的玻璃；
     * 另一个变体里玻璃自己也在动（这时脏区来自玻璃节点自身）。
     */
    const char *html = "<div id=r><div id=mover></div><div id=g></div></div>";
    const char *css =
        "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
        "#mover { position: absolute; left: 0px; top: 85px; width: 40px;"
        "         height: 40px; background-color: #0969DA;"
        "         animation: mv 600ms linear infinite alternate }"
        "@keyframes mv { from { transform: translate(0px, 0px); }"
        "                to   { transform: translate(120px, 0px); } }"
        "#g { position: absolute; left: 40px; top: 60px; width: 100px;"
        "     height: 80px; background-color: rgba(255,255,255,0.20);"
        "     backdrop-filter: blur(8px) }";

    QLUIDoc *doc = ql_ui_load_text(html, css);
    QL_CHECK(doc != NULL);
    if (doc != NULL) {
        QLUIRoot *ui = ql_ui_build(doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            ql_ui_layout(ui, W, H);

            QLDispDriver *dd = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
            QLCanvas dc;
            QL_CHECK_EQ(ql_canvas_from_disp(&dc, dd), QL_OK);
            QLDraw2D d2d;
            ql_draw2d_canvas_init(&d2d, &dc);

            ql_canvas_clear(&dc, 0xFFFFFF);
            ql_ui_render_canvas(ui, &dc);

            /* 丢掉首帧的脏区（首帧本来就是整屏画过的） */
            QLRect drop[16];
            (void)ql_ui_consume_dirty(ui, drop, 16);

            const int bad_cover = run_dirty(ui, &d2d, 8);
            const long diff = compare_with_full(ui, dd, W, H);
            printf("  下层在动：脏区未覆盖玻璃 %d 次，脏区重绘与整屏重绘差 %ld 像素\n",
                   bad_cover, diff);
            QL_CHECK_EQ(bad_cover, 0);
            QL_CHECK_EQ(diff, 0);

            ql_ui_free(ui);
            ql_disp_mem_destroy(dd);
        }
        ql_ui_doc_free(doc);
    }

    /* ---- 玻璃自己动：脏区来自玻璃节点本身，也必须仍是整块 ---- */
    {
        const char *html2 = "<div id=r><div id=mover></div><div id=g></div></div>";
        const char *css2 =
            "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
            "#mover { position: absolute; left: 0px; top: 85px; width: 40px;"
            "         height: 40px; background-color: #0969DA;"
            "         animation: mv 600ms linear infinite alternate }"
            "@keyframes mv { from { transform: translate(0px, 0px); }"
            "                to   { transform: translate(120px, 0px); } }"
            "#g { position: absolute; left: 40px; top: 40px; width: 60px;"
            "     height: 50px; background-color: rgba(255,255,255,0.20);"
            "     backdrop-filter: blur(6px);"
            "     animation: gm 900ms linear infinite alternate }"
            "@keyframes gm { from { transform: translate(0px, 0px); }"
            "                to   { transform: translate(20px, 30px); } }";
        QLUIDoc *doc2 = ql_ui_load_text(html2, css2);
        QL_CHECK(doc2 != NULL);
        if (doc2 != NULL) {
            QLUIRoot *ui2 = ql_ui_build(doc2);
            QL_CHECK(ui2 != NULL);
            if (ui2 != NULL) {
                ql_ui_layout(ui2, W, H);
                QLDispDriver *dd = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
                QLCanvas dc;
                QL_CHECK_EQ(ql_canvas_from_disp(&dc, dd), QL_OK);
                QLDraw2D d2d;
                ql_draw2d_canvas_init(&d2d, &dc);
                ql_canvas_clear(&dc, 0xFFFFFF);
                ql_ui_render_canvas(ui2, &dc);
                QLRect drop[16];
                (void)ql_ui_consume_dirty(ui2, drop, 16);

                const int bad_cover = run_dirty(ui2, &d2d, 8);
                const long diff = compare_with_full(ui2, dd, W, H);
                printf("  玻璃在动：脏区未覆盖玻璃 %d 次，差 %ld 像素\n", bad_cover, diff);
                QL_CHECK_EQ(bad_cover, 0);
                QL_CHECK_EQ(diff, 0);

                ql_ui_free(ui2);
                ql_disp_mem_destroy(dd);
            }
            ql_ui_doc_free(doc2);
        }
    }

QL_TEST_END()
