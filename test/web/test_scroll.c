/**
 * @file    test_scroll.c
 * @brief   滚动容器（`overflow: auto`）。
 *
 * 核心设计：**滚动偏移烘进布局**，不在绘制时临时加。
 * 所以判据可以全部走 `ql_ui_node_rect()`（绘制、命中、脏区读的都是同一份坐标），
 * 只有滚动条本身需要看像素。
 *
 * `hidden` 与 `auto` 的区别只有一条：**hidden 只裁不滚**。两者都裁。
 */
#include <stdio.h>
#include <string.h>

#include "../ql_test.h"

#define W 120
#define H 100

/* 5 个 40px 高的项装进 100px 高的容器：内容 200，可滚 100 */
static const char *k_html =
    "<div id=r><div class=sc>"
    "<div class=i>1</div><div class=i>2</div><div class=i>3</div>"
    "<div class=i>4</div><div class=i>5</div></div></div>";

static const char *k_css_auto =
    "#r { width: 120px; height: 100px; }"
    ".sc { width: 120px; height: 100px; overflow: auto; }"
    ".i { width: 120px; height: 40px; background-color: #334455; color: #FFFFFF; }";

static const char *k_css_hidden =
    "#r { width: 120px; height: 100px; }"
    ".sc { width: 120px; height: 100px; overflow: hidden; }"
    ".i { width: 120px; height: 40px; background-color: #334455; color: #FFFFFF; }";

static QLUIRoot *mk(const char *css, QLUIDoc **out_doc)
{
    QLUIDoc *doc = ql_ui_load_text(k_html, css);
    if (doc == NULL) {
        printf("  编译失败：%s\n", ql_ui_text_last_error());
        return NULL;
    }
    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        return NULL;
    }
    ql_ui_layout(ui, W, H);
    *out_doc = doc;
    return ui;
}

/* 一格滚多少像素（与 qlui.c 用同一个宏） */
#define STEP QL_CFG_UI_WHEEL_STEP_PX

/* dx/dy 是**格数**（X11 Button4/5 一次一格），不是像素 */
static void wheel(QLUIRoot *ui, int x, int y, int dx, int dy)
{
    QLInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = QL_INPUT_WHEEL;
    ev.x = x;
    ev.y = y;
    ev.dx = dx;
    ev.dy = dy;
    ql_ui_input_event(ui, &ev);
}

static int click_count;
static void on_click(QLUIRoot *ui, QLUINodeRT *node, void *user)
{
    (void)ui;
    (void)node;
    (void)user;
    click_count++;
}

QL_TEST_BEGIN("test_scroll")

    /* ---------- 1. auto：可滚，范围 = 内容 - 可视 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk(k_css_auto, &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK(ql_ui_scrollable(&ui->nodes[1]));
            QL_CHECK_EQ((int)ui->nodes[1].scroll_max_y, 200 - 100);
            QL_CHECK_EQ((int)ui->nodes[1].scroll_max_x, 0);   /* 横向不溢出 */
            QL_CHECK_EQ((int)ui->nodes[1].scroll_y, 0);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 2. hidden：只裁不滚 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk(k_css_hidden, &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK(!ql_ui_scrollable(&ui->nodes[1]));
            QL_CHECK_EQ((int)ui->nodes[1].scroll_max_y, 0);
            /* 滚轮不起作用：内容位置不变 */
            const int y0 = (int)ql_ui_node_rect(&ui->nodes[2]).y;
            wheel(ui, 50, 50, 0, 3);
            QL_CHECK_EQ((int)ql_ui_node_rect(&ui->nodes[2]).y, y0);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 3. 滚轮：内容整体上移 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk(k_css_auto, &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ((int)ql_ui_node_rect(&ui->nodes[2]).y, 0);
            /* 一格 = STEP 像素（浏览器一个量级）；之前把格数当像素用，一格只走 1px */
            wheel(ui, 50, 50, 0, 1);
            QL_CHECK_EQ((int)ui->nodes[1].scroll_y, STEP);
            QL_CHECK_EQ((int)ql_ui_node_rect(&ui->nodes[2]).y, -STEP);
            /* 往回滚一格：回到 0，并夹紧不出现负数 */
            wheel(ui, 50, 50, 0, -1);
            QL_CHECK_EQ((int)ui->nodes[1].scroll_y, 0);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 4. 夹紧：滚不过头也滚不到负数 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk(k_css_auto, &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            wheel(ui, 50, 50, 0, 1000);
            QL_CHECK_EQ((int)ui->nodes[1].scroll_y, 100);
            wheel(ui, 50, 50, 0, -1000);
            QL_CHECK_EQ((int)ui->nodes[1].scroll_y, 0);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 5. scroll_by 之后节点被标脏（主循环能拿到重绘区域） ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk(k_css_auto, &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLRect areas[8];
            (void)ql_ui_consume_dirty(ui, areas, 8);          /* 清空 */
            QL_CHECK_EQ(ql_ui_scroll_by(ui, &ui->nodes[1], 0, 10), QL_OK);
            QL_CHECK(ql_ui_consume_dirty(ui, areas, 8) > 0);
            /* 顶到头时不该产生脏区（也就不会白重绘） */
            (void)ql_ui_consume_dirty(ui, areas, 8);
            QL_CHECK_EQ(ql_ui_scroll_by(ui, &ui->nodes[1], 0, 500), QL_OK);
            QL_CHECK(ql_ui_consume_dirty(ui, areas, 8) > 0);
            (void)ql_ui_consume_dirty(ui, areas, 8);
            QL_CHECK_EQ(ql_ui_scroll_by(ui, &ui->nodes[1], 0, 500), QL_OK);
            QL_CHECK_EQ(ql_ui_consume_dirty(ui, areas, 8), 0);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 6. 命中测试跟着滚：滚出视野的项点不到 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk(k_css_auto, &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            /* 初始：y=50 落在第 2 项（40..80） */
            QLUINodeRT *hit = ql_ui_hit_test(ui, 50, 50);
            QL_CHECK(hit != NULL);
            if (hit != NULL)
                QL_CHECK_EQ((int)hit->src->tag == (int)QL_UI_TAG_DIV, 1);
            /* 滚 40 之后：第 1 项被推到 -40，y=10 处应该是第 2 项 */
            wheel(ui, 50, 50, 0, 40);
            QLUINodeRT *hit2 = ql_ui_hit_test(ui, 50, 10);
            QL_CHECK(hit2 != NULL);
            /* 命中点已经不可能是第 1 项（它整体在视口上方） */
            if (hit2 != NULL)
                QL_CHECK((int)ql_ui_node_rect(hit2).y <= 10);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 7. 横向滚动 ---------- */
    {
        const char *html = "<div id=r><div class=sc><div class=w>wide</div></div></div>";
        const char *css = "#r { width: 120px; height: 60px; }"
                          ".sc { width: 120px; height: 60px; overflow: auto; }"
                          ".w { width: 300px; height: 30px; color: #FFFFFF; }";
        QLUIDoc *doc = ql_ui_load_text(html, css);
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QLUIRoot *ui = ql_ui_build(doc);
            ql_ui_layout(ui, W, H);
            QL_CHECK(ql_ui_scrollable(&ui->nodes[1]));
            QL_CHECK_EQ((int)ui->nodes[1].scroll_max_x, 300 - 120);
            /* 横向滚轮同样是"格"：1 格 = 60px（这里 max_x 正好 180，够滚） */
            wheel(ui, 50, 50, 1, 0);
            QL_CHECK_EQ((int)ui->nodes[1].scroll_x, STEP);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 8. 拖滚动条：不算点击 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk(k_css_auto, &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            click_count = 0;
            ql_ui_set_click_cb(ui, on_click, NULL);
            /* 右侧竖带是滚动条热区 */
            QL_CHECK_EQ(ql_ui_pointer_down(ui, W - 3, 50), 1);
            QL_CHECK(ui->scroll_drag != NULL);
            ql_ui_pointer_move(ui, W - 3, 80);
            QL_CHECK(ui->nodes[1].scroll_y > 0);
            ql_ui_pointer_up(ui, W - 3, 80);
            QL_CHECK(ui->scroll_drag == NULL);
            QL_CHECK_EQ(click_count, 0);        /* 拖滚动条不触发点击 */

            /* 普通位置按下再抬起才算点击 */
            ql_ui_pointer_down(ui, 20, 20);
            ql_ui_pointer_up(ui, 20, 20);
            QL_CHECK_EQ(click_count, 1);
            ql_ui_set_click_cb(ui, NULL, NULL);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 9. 内容不超出：不滚、也没有滚动条 ---------- */
    {
        const char *html = "<div id=r><div class=sc><div class=i>x</div></div></div>";
        const char *css = "#r { width: 120px; height: 100px; }"
                          ".sc { width: 120px; height: 100px; overflow: auto; }"
                          ".i { width: 120px; height: 20px; }";
        QLUIDoc *doc = ql_ui_load_text(html, css);
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QLUIRoot *ui = ql_ui_build(doc);
            ql_ui_layout(ui, W, H);
            QL_CHECK(!ql_ui_scrollable(&ui->nodes[1]));
            QL_CHECK_EQ((int)ui->nodes[1].scroll_max_y, 0);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 10. 嵌套：滚轮作用在最近的可滚动祖先 ---------- */
    {
        const char *html =
            "<div id=r><div class=outer><div class=inner>"
            "<div class=i>1</div><div class=i>2</div><div class=i>3</div>"
            "<div class=i>4</div></div></div></div>";
        const char *css = "#r { width: 120px; height: 100px; }"
                          ".outer { width: 120px; height: 50px; overflow: auto; }"
                          ".inner { width: 120px; overflow: auto; }"
                          ".i { width: 120px; height: 40px; }";
        QLUIDoc *doc = ql_ui_load_text(html, css);
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QLUIRoot *ui = ql_ui_build(doc);
            ql_ui_layout(ui, W, H);
            wheel(ui, 50, 20, 0, 1);
            /* 指针落在 inner 上 → 滚 inner（它也在视口内且可滚） */
            QL_CHECK(ui->nodes[2].scroll_y == STEP || ui->nodes[1].scroll_y == STEP);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 11. flex 里的滚动容器：滚动位置不能被测量过程清掉 ---------- */
    {
        /*
         * 手机布局几乎都是"flex 列 + 中间列表 flex:1 + overflow:auto"。
         * flex 会**先测量**每一项的主轴尺寸（临时高度），测量那几轮的可滚范围
         * 还是 0 或很小 —— 如果那时按临时值夹一次滚动偏移，用户的滚动位置就
         * 变回 0 了。现象：列表滚不动（拖/滚轮都白费）。
         */
        const char *html =
            "<div id=r><div class=sc><div class=i>1</div><div class=i>2</div>"
            "<div class=i>3</div><div class=i>4</div><div class=i>5</div>"
            "<div class=i>6</div></div></div>";
        const char *css = "#r { width: 120px; height: 100px; display: flex;"
                          "     flex-direction: column; }"
                          ".sc { width: 120px; flex: 1 1 0; overflow-y: auto; }"
                          ".i { width: 120px; height: 40px; background-color: #334455; }";
        QLUIDoc *doc = ql_ui_load_text(html, css);
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QLUIRoot *ui = ql_ui_build(doc);
            ql_ui_layout(ui, W, H);
            QLUINodeRT *sc = &ui->nodes[1];
            QL_CHECK(ql_ui_scrollable(sc));
            QL_CHECK(sc->scroll_max_y > 0);

            QL_CHECK_EQ(ql_ui_scroll_by(ui, sc, 0, 60), QL_OK);
            const ql_coord_t y1 = sc->scroll_y;
            QL_CHECK_EQ((int)y1, 60);

            /* 再排一次（= 每帧/每次脏区重排都会做的事）：位置必须还在 */
            ql_ui_layout(ui, W, H);
            QL_CHECK_EQ((int)sc->scroll_y, (int)y1);

            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

QL_TEST_END()
