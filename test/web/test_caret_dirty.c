/**
 * @file    test_caret_dirty.c
 * @brief   脏区模式下"焦点换了，旧输入框的光标必须消失"。
 *
 * 现象（用户报的）：点几个输入框之后屏幕上出现**好几个光标**，而且越点越多。
 *
 * 原因：光标只在 `ui->focus == node` 时画，而焦点变化走的是 `set_state_chain()`
 * 改状态位 —— 如果那里不标脏，旧输入框根本不会被重画，它那个 1px 竖线就一直
 * 留在保留式画布上。这个 bug 在"整屏重绘"模式下完全看不出来，只有脏区重绘
 * （窗口后端 + `--retained`）才会显现，所以原来 84 个测试全绿也没抓到。
 *
 * 判据：一路只用脏区重绘画出来的画面，必须与"整屏重绘"逐像素一致。
 */
#include "../ql_test.h"

#define W 220
#define H 90

static void send_text(QLUIRoot *ui, const char *s)
{
    QLInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = QL_INPUT_TEXT;
    snprintf(ev.text, sizeof(ev.text), "%s", s);
    ql_ui_input_event(ui, &ev);
}

static void send_key(QLUIRoot *ui, uint32_t key, uint8_t mods)
{
    QLInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = QL_INPUT_KEY_DOWN;
    ev.key = key;
    ev.mods = mods;
    ql_ui_input_event(ui, &ev);
}

/* 点一下（按下 + 抬起），坐标是 CSS px */
static void click(QLUIRoot *ui, ql_coord_t x, ql_coord_t y)
{
    (void)ql_ui_pointer_down(ui, x, y);
    (void)ql_ui_pointer_up(ui, x, y);
}

/*
 * 只画脏区（与窗口主循环同一条路）。
 *
 * `ql_draw2d_canvas_init()` 背后是**静态上下文**，同一时刻只有最近 init 的那块
 * 画布有效 —— 所以这里每次都重新 init 到目标画布，而不是抱着一个 d2d 不放
 * （中间会有别的画布被 init 过，比如整屏对比那张）。
 */
static int paint_dirty(QLUIRoot *ui, QLCanvas *dc)
{
    QLDraw2D d2d;
    ql_draw2d_canvas_init(&d2d, dc);
    QLRect areas[16];
    const int n = ql_ui_consume_dirty(ui, areas, 16);
    for (int i = 0; i < n; i++)
        ql_ui_render_rect(ui, &d2d, areas[i]);
    return n;
}

/* 整屏重绘到另一块画布，返回与 dirty 画布不同的像素数 */
static long diff_full(QLUIRoot *ui, QLDispDriver *dirty, int w, int h)
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
    int bx0 = w, by0 = h, bx1 = -1, by1 = -1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (ql_disp_mem_pixel(dirty, x, y) != ql_disp_mem_pixel(fd, x, y)) {
                diff++;
                if (x < bx0) bx0 = x;
                if (y < by0) by0 = y;
                if (x > bx1) bx1 = x;
                if (y > by1) by1 = y;
            }
    if (diff > 0) {
        printf("    差异包围盒 (%d,%d)-(%d,%d)\n", bx0, by0, bx1, by1);
        ql_disp_mem_save_ppm(dirty, "/tmp/cd_dirty.ppm");
        ql_disp_mem_save_ppm(fd, "/tmp/cd_full.ppm");
    }
    ql_disp_mem_destroy(fd);
    return diff;
}

/*
 * 数一个输入框里"光标状"的竖线：某一列从内容盒顶部到底部几乎全是暗像素。
 * 字形不会有这种整列都暗的结构（'l' 也只在一条窄带里，且高度不足），
 * 所以这个判据能干净地区分"有没有光标"。
 */
static int caret_ink(QLDispDriver *d, QLRect box)
{
    const ql_coord_t y0 = box.y + 3, y1 = ql_rect_bottom(box) - 3;
    const int span = (int)(y1 - y0);
    if (span <= 0)
        return 0;
    int cols = 0;
    for (ql_coord_t x = box.x; x < ql_rect_right(box); x++) {
        int dark = 0, bright_after = 0;
        for (ql_coord_t y = y0; y < y1; y++) {
            const uint32_t v = ql_disp_mem_pixel(d, x, y) & 0xFFFFFFu;
            if (v < 0x606060u)
                dark++;
            else
                bright_after++;
        }
        (void)bright_after;
        if (dark >= span * 3 / 4)
            cols++;
    }
    return cols;
}

QL_TEST_BEGIN("test_caret_dirty")

    /* 三个空输入框：这样框里唯一的整列暗像素就是光标（不会被字的竖笔误导） */
    const char *html = "<div id=r><input id=a><input id=b><input id=c></div>";
    const char *css = "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
                      "input { display: block; width: 180px; height: 22px;"
                      "        margin: 6px 8px; font-size: 14px }";

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
            /* 首帧整屏画一遍（与窗口主循环一致：need_full 的第一次） */
            ql_canvas_clear(&dc, 0xFFFFFF);
            ql_ui_render_canvas(ui, &dc);
            QLRect drop[16];
            (void)ql_ui_consume_dirty(ui, drop, 16);

            /* 三个输入框按文档顺序取 */
            QLUINodeRT *a = NULL, *b = NULL, *c = NULL;
            for (uint16_t i = 0; i < ui->node_count; i++) {
                const QLUINodeRT *nd = &ui->nodes[i];
                if (nd->src == NULL || nd->src->tag != QL_UI_TAG_INPUT)
                    continue;
                if (a == NULL)
                    a = &ui->nodes[i];
                else if (b == NULL)
                    b = &ui->nodes[i];
                else if (c == NULL)
                    c = &ui->nodes[i];
            }
            QL_CHECK(a != NULL && b != NULL && c != NULL);

            if (a != NULL && b != NULL && c != NULL) {
                /* 1. 点进 a 并打字，只画脏区 */
                click(ui, a->content_box.x + 2, a->content_box.y + 4);
                send_text(ui, "xy");
                QL_CHECK_EQ(ql_ui_focus_node(ui), a);
                (void)paint_dirty(ui, &dc);
                QL_CHECK(caret_ink(dd, a->content_box) > 0);   /* a 有光标 */

                /*
                 * 2. 点到 b：a 失焦。这一步以前**不标脏**，于是 a 的光标
                 *    永远留在屏幕上 —— 用户看到"两个光标"。
                 */
                click(ui, b->content_box.x + 2, b->content_box.y + 4);
                QL_CHECK_EQ(ql_ui_focus_node(ui), b);
                (void)paint_dirty(ui, &dc);
                QL_CHECK_EQ(caret_ink(dd, a->content_box), 0);  /* a 的光标没了 */
                QL_CHECK(caret_ink(dd, b->content_box) > 0);
                /* 打字必须进**新**焦点（而不是留在旧的那个） */
                send_text(ui, "Q");
                /* 光标像素判据只用在"空框"上：框里一旦有字，字的竖笔会误判 */
                QL_CHECK_STR(ql_ui_input_value(ui, a), "xy");
                QL_CHECK_STR(ql_ui_input_value(ui, b), "Q");
                (void)paint_dirty(ui, &dc);
                QL_CHECK_EQ(diff_full(ui, dd, W, H), 0);

                /* 3. Tab 到 c（键盘路径），再打一个字 */
                send_key(ui, QL_KEY_TAB, 0);
                QL_CHECK_EQ(ql_ui_focus_node(ui), c);
                (void)paint_dirty(ui, &dc);
                QL_CHECK(caret_ink(dd, c->content_box) > 0);
                send_text(ui, "Z");
                QL_CHECK_STR(ql_ui_input_value(ui, c), "Z");
                (void)paint_dirty(ui, &dc);

                /* 4. 整体一致性：脏区重绘的结果必须等于整屏重绘 */
                const long diff = diff_full(ui, dd, W, H);
                printf("  脏区重绘 vs 整屏重绘：差 %ld 像素\n", diff);
                QL_CHECK_EQ(diff, 0);

                /* 5. 点空白处失焦：所有光标都消失 */
                click(ui, W - 4, H - 4);
                QL_CHECK(ql_ui_focus_node(ui) == NULL);
                (void)paint_dirty(ui, &dc);
                QL_CHECK_EQ(diff_full(ui, dd, W, H), 0);
            }

            ql_ui_free(ui);
            ql_disp_mem_destroy(dd);
        }
        ql_ui_doc_free(doc);
    }

QL_TEST_END()
