/**
 * @file    test_b5.c
 * @brief   B5 小件：`visibility` / `pointer-events` / `align-self` / `order` /
 *          `flex-flow` 简写。
 *
 * 这五个的共同点：都**不动布局大框架**，只是给已有机制加一个开关 ——
 *
 *   visibility   占位但不画自己（`display: none` 是不占位），且**继承**
 *   pointer-events  自己不参与命中，子节点照常（盖在按钮上的装饰层不该挡点击）
 *   align-self   覆盖自己在容器交叉轴上的对齐（`auto` = 跟容器的 align-items）
 *   order        flex 项顺序；就地排子链表，绘制/命中自动跟着走
 *   flex-flow    `flex-direction` + `flex-wrap` 的简写，两个 token 任意顺序
 *
 * 除了 align-self/order 走布局，其余三个都可以只用"渲染结果 + 命中测试"验证。
 */
#include <stdlib.h>

#include "../ql_test.h"

#define CW 160
#define CH 120
#define BG 0xFFFFFFu

static QLDispDriver *s_disp;
static QLCanvas s_canvas;
static QLUIDoc *s_doc;

static QLUIRoot *render(const char *html, const char *css)
{
    QLUIDoc *doc = ql_ui_load_text(html, css);
    if (doc == NULL) {
        printf("  编译失败：%s\n", ql_ui_text_last_error());
        return NULL;
    }
    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        return NULL;
    }
    s_doc = doc;
    ql_ui_layout(ui, CW, CH);
    if (s_disp == NULL)
        s_disp = ql_disp_mem_create(CW, CH, QL_COLOR_RGB888);
    ql_canvas_from_disp(&s_canvas, s_disp);
    ql_canvas_clear(&s_canvas, BG);
    ql_ui_render_canvas(ui, &s_canvas);
    return ui;
}

static void release(QLUIRoot *ui)
{
    ql_ui_free(ui);
    if (s_doc != NULL) {
        ql_ui_doc_free(s_doc);
        s_doc = NULL;
    }
}

static ql_color_t px(int x, int y)
{
    return ql_disp_mem_pixel(s_disp, x, y);
}

static int is_bg(ql_color_t c)
{
    return QL_COLOR_GET_R(c) == 0xFF && QL_COLOR_GET_G(c) == 0xFF &&
           QL_COLOR_GET_B(c) == 0xFF;
}

static int is_blue(ql_color_t c)
{
    return QL_COLOR_GET_B(c) > 200 && QL_COLOR_GET_R(c) < 60;
}

/* 第 idx 个节点的边框盒 */
static QLRect rect_of(QLUIRoot *ui, int idx)
{
    return ql_ui_node_rect(&ui->nodes[idx]);
}

static int rejects(const char *css, const char *needle)
{
    const char *H = "<div id=r><div id=a>x</div></div>";
    QLUIDoc *doc = ql_ui_load_text(H, css);
    if (doc != NULL) {
        ql_ui_doc_free(doc);
        printf("  本该报错却编译通过了：%s\n", css);
        return 0;
    }
    const char *msg = ql_ui_text_last_error();
    if (msg == NULL || strstr(msg, needle) == NULL) {
        printf("  报错文案不含 '%s'：%s\n", needle, msg ? msg : "(null)");
        return 0;
    }
    return 1;
}

QL_TEST_BEGIN("test_b5")

    /* 白底 + 一个蓝块。盒子位置固定，方便逐点取色。 */
    const char *H = "<div id=r><div id=b></div></div>";
    const char *BASE =
        "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
        "#b { position: absolute; left: 20px; top: 20px; width: 40px; height: 40px;"
        "     background-color: #0000FF }";

    /* ---------- 1. visibility: visible 与不写等价 ---------- */
    QLUIRoot *ui = render(H, BASE);
    QL_CHECK(ui != NULL);
    QL_CHECK(is_blue(px(40, 40)));
    release(ui);

    ui = render(H, "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
                   "#b { position: absolute; left: 20px; top: 20px; width: 40px;"
                   "     height: 40px; background-color: #0000FF;"
                   "     visibility: visible }");
    QL_CHECK(ui != NULL);
    QL_CHECK(is_blue(px(40, 40)));
    release(ui);

    /* ---------- 2. visibility: hidden：不画自己，但**照样占位** ---------- */
    ui = render(H, "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
                   "#b { position: absolute; left: 20px; top: 20px; width: 40px;"
                   "     height: 40px; background-color: #0000FF;"
                   "     visibility: hidden }");
    QL_CHECK(ui != NULL);
    QL_CHECK(is_bg(px(40, 40)));            /* 没画 */
    {
        const QLRect r = rect_of(ui, 1);
        QL_CHECK_EQ(r.w, 40);               /* 但布局还在（display:none 才是 0） */
        QL_CHECK_EQ(r.h, 40);
    }
    /* 命中测试也不认它 */
    QL_CHECK(ql_ui_hit_test(ui, 40, 40) != &ui->nodes[1]);
    release(ui);

    /* ---------- 3. visibility 是继承属性，子级可以自己 visible 回来 ---------- */
    ui = render("<div id=r><div id=p><div id=c></div></div></div>",
                "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
                "#p { position: absolute; left: 10px; top: 10px; width: 100px;"
                "     height: 60px; background-color: #0000FF;"
                "     visibility: hidden }"
                "#c { position: absolute; left: 10px; top: 10px; width: 20px;"
                "     height: 20px; background-color: #0000FF;"
                "     visibility: visible }");
    QL_CHECK(ui != NULL);
    QL_CHECK(is_bg(px(60, 40)));            /* 父级 hidden：没画 */
    QL_CHECK(is_blue(px(25, 25)));          /* 子级 visible：画了 */
    release(ui);

    /* ---------- 4. pointer-events: none 只影响自己 ---------- */
    ui = render(H, BASE);
    QL_CHECK(ui != NULL);
    QL_CHECK_EQ(ql_ui_hit_test(ui, 40, 40), &ui->nodes[1]);
    release(ui);

    ui = render(H, "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
                   "#b { position: absolute; left: 20px; top: 20px; width: 40px;"
                   "     height: 40px; background-color: #0000FF;"
                   "     pointer-events: none }");
    QL_CHECK(ui != NULL);
    QL_CHECK(is_blue(px(40, 40)));          /* 照样画 */
    QL_CHECK(ql_ui_hit_test(ui, 40, 40) != &ui->nodes[1]);   /* 但点不中 */
    release(ui);

    /* 子节点写 auto 就能重新接住点击（装饰层不挡按钮的标准做法） */
    ui = render("<div id=r><div id=o><div id=b></div></div></div>",
                "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
                "#o { position: absolute; left: 10px; top: 10px; width: 100px;"
                "     height: 60px; background-color: #0000FF;"
                "     pointer-events: none }"
                "#b { position: absolute; left: 10px; top: 10px; width: 30px;"
                "     height: 30px; background-color: #0000FF;"
                "     pointer-events: auto }");
    QL_CHECK(ui != NULL);
    QL_CHECK(ql_ui_hit_test(ui, 25, 25) == &ui->nodes[2]);   /* 子节点命中 */
    QL_CHECK(ql_ui_hit_test(ui, 90, 50) != &ui->nodes[1]);   /* 覆盖层自己不命中 */
    release(ui);

    /* ---------- 5. align-self：覆盖容器的 align-items ---------- */
    {
        const char *FH = "<div id=r><div id=a></div><div id=b></div></div>";
        const char *CSS =
            "#r { display: flex; align-items: flex-start; width: 200px; height: 100px; }"
            "#a { width: 20px; height: 10px; }"
            "#b { width: 20px; height: 10px; align-self: flex-end; }";
        ui = render(FH, CSS);
        QL_CHECK(ui != NULL);
        QL_CHECK_EQ(rect_of(ui, 1).y, 0);           /* 跟随 align-items: flex-start */
        QL_CHECK_EQ(rect_of(ui, 2).y, 90);          /* align-self 压到底 */
        release(ui);

        /* center / stretch */
        ui = render(FH,
                    "#r { display: flex; align-items: flex-start; width: 200px;"
                    "     height: 100px; }"
                    "#a { width: 20px; height: 10px; align-self: center; }"
                    "#b { width: 20px; align-self: stretch; }");
        QL_CHECK(ui != NULL);
        QL_CHECK_EQ(rect_of(ui, 1).y, 45);          /* (100-10)/2 */
        QL_CHECK_EQ(rect_of(ui, 2).h, 100);         /* stretch 撑满一行 */
        release(ui);

        /* auto 与不写等价 */
        ui = render(FH,
                    "#r { display: flex; align-items: center; width: 200px;"
                    "     height: 100px; }"
                    "#a { width: 20px; height: 10px; }"
                    "#b { width: 20px; height: 10px; align-self: auto; }");
        QL_CHECK(ui != NULL);
        QL_CHECK_EQ(rect_of(ui, 1).y, 45);
        QL_CHECK_EQ(rect_of(ui, 2).y, 45);
        release(ui);
    }

    /* ---------- 6. order：换排列顺序，绘制与命中一起跟着走 ---------- */
    {
        const char *OH = "<div id=r><div id=a></div><div id=b></div><div id=c></div></div>";
        const char *CSS =
            "#r { display: flex; width: 300px; height: 100px; }"
            "#a { width: 30px; height: 10px; }"
            "#b { width: 30px; height: 10px; }"
            "#c { width: 30px; height: 10px; }";
        ui = render(OH, CSS);
        QL_CHECK(ui != NULL);
        QL_CHECK_EQ(rect_of(ui, 1).x, 0);           /* 默认顺序 */
        QL_CHECK_EQ(rect_of(ui, 2).x, 30);
        QL_CHECK_EQ(rect_of(ui, 3).x, 60);
        release(ui);

        /* c 提到最前、a 放到最后；b 没写 order = 0 */
        char css[512];
        snprintf(css, sizeof(css),
                 "%s #c { order: -1; } #a { order: 1; }", CSS);
        ui = render(OH, css);
        QL_CHECK(ui != NULL);
        QL_CHECK_EQ(rect_of(ui, 3).x, 0);           /* c 第一 */
        QL_CHECK_EQ(rect_of(ui, 2).x, 30);          /* b 第二（order 0） */
        QL_CHECK_EQ(rect_of(ui, 1).x, 60);          /* a 最后 */
        release(ui);

        /* order 相等时保持 HTML 顺序（稳定） */
        snprintf(css, sizeof(css),
                 "%s #a { order: 2; } #b { order: 2; } #c { order: 1; }", CSS);
        ui = render(OH, css);
        QL_CHECK(ui != NULL);
        QL_CHECK_EQ(rect_of(ui, 3).x, 0);
        QL_CHECK_EQ(rect_of(ui, 1).x, 30);
        QL_CHECK_EQ(rect_of(ui, 2).x, 60);
        release(ui);
    }

    /* ---------- 7. flex-flow 简写 ---------- */
    {
        const char *FH = "<div id=r><div id=a></div><div id=b></div></div>";
        /* 两个 token 任意顺序 */
        ui = render(FH,
                    "#r { display: flex; flex-flow: column wrap; width: 100px;"
                    "     height: 200px; }"
                    "#a { width: 20px; height: 20px; }"
                    "#b { width: 20px; height: 20px; }");
        QL_CHECK(ui != NULL);
        QL_CHECK_EQ(rect_of(ui, 1).x, 0);
        QL_CHECK_EQ(rect_of(ui, 2).y, 20);          /* 竖排 */
        release(ui);

        ui = render(FH,
                    "#r { display: flex; flex-flow: wrap column; width: 100px;"
                    "     height: 200px; }"
                    "#a { width: 20px; height: 20px; }"
                    "#b { width: 20px; height: 20px; }");
        QL_CHECK(ui != NULL);
        QL_CHECK_EQ(rect_of(ui, 2).y, 20);
        release(ui);

        /* 只写一个值也合法 */
        ui = render(FH,
                    "#r { display: flex; flex-flow: column; width: 100px; height: 200px; }"
                    "#a { width: 20px; height: 20px; }"
                    "#b { width: 20px; height: 20px; }");
        QL_CHECK(ui != NULL);
        QL_CHECK_EQ(rect_of(ui, 2).y, 20);
        release(ui);
    }

    /* ---------- 8. 构建期必须报错的写法 ---------- */
    QL_CHECK(rejects("#a { visibility: collapse-x }", "visibility"));
    QL_CHECK(rejects("#a { pointer-events: visible }", "pointer-events"));
    QL_CHECK(rejects("#a { align-self: middle }", "align-self"));
    QL_CHECK(rejects("#a { order: 1.5 }", "整数"));
    QL_CHECK(rejects("#a { order: 99999 }", "超出"));
    QL_CHECK(rejects("#a { flex-flow: sideways }", "flex-flow"));

    if (s_disp != NULL) {
        ql_disp_mem_destroy(s_disp);
        s_disp = NULL;
    }

QL_TEST_END()
