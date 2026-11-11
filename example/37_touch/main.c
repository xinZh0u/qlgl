/**
 * @file    main.c
 * @brief   触摸：单指点按 / 单指拖动滚动 / 双指捏合缩放整页。
 *
 *   ./build/example/example_37_touch                      # 打开窗口（触摸屏或鼠标）
 *   ./build/example/example_37_touch --dump /tmp/t.ppm    # 截图（zoom 100%）
 *
 * 触摸是**后端原样上报、界面层认手势**：
 *
 *   1. Wayland 的 `wl_touch` 只推 `QL_INPUT_TOUCH`（哪根手指、几点、还有几根），
 *      不在后端合成鼠标事件 —— 因为"这一下算点按还是算拖动/捏合"要看按在谁身上；
 *   2. 界面层：单指 = 鼠标（松开合成 click）；单指在可滚动容器里移动超过
 *      `QL_CFG_UI_TOUCH_SLOP`（8px）就改成拖内容滚动，且不算点击；
 *      第二根手指落下就把那次按下**作废**（没有 click）并转入捏合；
 *   3. 捏合只回调 `(中心, 倍数)`，**缩放什么由应用决定** —— 这里缩放整页：
 *      布局按 `窗口 / zoom`，绘制由 `ql_gfx_gl_set_zoom()` 放大，
 *      输入坐标后端已经除掉 zoom，所以界面层永远只看到自己那套 CSS 坐标。
 *
 * 鼠标行为完全不变：滚轮滚列表、拖滚动条，捏合（双指）鼠标做不出来。
 */
#include "common.h"

#define W 340
#define H 600

/* 缩放范围：50% ~ 300%（再小看不清，再大布局开销没意义） */
#define ZOOM_MIN (QL_SCALE_ONE / 2)
#define ZOOM_MAX (QL_SCALE_ONE * 3)

static QLUIRoot *s_ui;
static QLUINodeRT *s_cnt;
static QLUINodeRT *s_zoom_label;
static QLUINodeRT *s_status;
static QLUINodeRT *s_list;

static QLGfxGL  *s_gfx;
static ql_coord_t s_win_w = W, s_win_h = H;
static ql_coord_t s_zoom = QL_SCALE_ONE;   /* 当前缩放（26.6） */
static ql_coord_t s_zoom0 = QL_SCALE_ONE;  /* 捏合开始时的缩放 */
static int        s_taps;
static int        s_redraw;                /* 下一帧整屏重绘（缩放变了） */
static int        s_pending_zoom;          /* 钩子还没拿到的缩放请求 */

/*
 * `ql_ui_set_text()` **不复制**：缓冲必须一直有效，所以这两个用静态的。
 * （踩过：传栈上局部数组 → 状态栏变成"0,□:□□"这种乱码。）
 */
static char s_status_buf[96];

static void set_status(const char *what)
{
    snprintf(s_status_buf, sizeof(s_status_buf), "%s（点按 %d 次）", what, s_taps);
    ql_ui_set_text(s_ui, s_status, s_status_buf);
}

/*
 * 布局基准 = 窗口 / zoom。
 *
 * 为什么不让库里自己除：缩放是应用的决定（也可能是"只缩放某个元素"），
 * 库只知道"画多大"，不知道"布局多大"。这里一行就够，不用多一个状态。
 */
static void layout_zoomed(void)
{
    const ql_coord_t lw = (ql_coord_t)((int64_t)s_win_w * QL_SCALE_ONE / s_zoom);
    const ql_coord_t lh = (ql_coord_t)((int64_t)s_win_h * QL_SCALE_ONE / s_zoom);
    ql_ui_layout(s_ui, lw, lh);
}

static ql_coord_t clamp_zoom(ql_coord_t z)
{
    if (z < ZOOM_MIN)
        z = ZOOM_MIN;
    if (z > ZOOM_MAX)
        z = ZOOM_MAX;
    return z;
}

/* 真的落一次缩放：告诉后端、按新基准重排、标脏、更新标签 */
static void apply_zoom_now(ql_coord_t z)
{
    s_zoom = clamp_zoom(z);
    if (s_gfx == NULL) {
        s_pending_zoom = 1;      /* 第一帧还没到（拿不到 gfx），钩子里再落 */
        return;
    }
    ql_gfx_gl_set_zoom(s_gfx, s_zoom);
    layout_zoomed();
    s_redraw = 1;

    static char pct[16];
    snprintf(pct, sizeof(pct), "%d%%", (int)((int64_t)s_zoom * 100 / QL_SCALE_ONE));
    ql_ui_set_text(s_ui, s_zoom_label, pct);
}

static void apply_zoom(ql_coord_t z)
{
    z = clamp_zoom(z);
    if (z == s_zoom)
        return;
    apply_zoom_now(z);
}

static void on_click(QLUIRoot *ui, QLUINodeRT *node, void *user)
{
    (void)user;
    /* 只有那个大按钮计次（列表行也行，但没必要） */
    if (node->src == NULL || node->src->tag != QL_UI_TAG_DIV)
        return;
    s_taps++;
    static char cnt[48];
    snprintf(cnt, sizeof(cnt), "点按 %d 次", s_taps);
    ql_ui_set_text(ui, s_cnt, cnt);
    set_status("点按 / 松手算一次点击");
    printf("%s\n", cnt);
}

/*
 * 捏合：库只给"倍数"，这里把它落到整页缩放上。
 *
 * BEGIN 时记下基准 —— 手势给的是**相对开始**的倍数，不是增量；
 * 用增量会在手指抖动时累积漂移。
 */
static void on_pinch(QLUIRoot *ui, ql_coord_t cx, ql_coord_t cy,
                     ql_coord_t scale_fx, int phase, void *user)
{
    (void)ui; (void)cx; (void)cy; (void)user;
    if (phase == QL_UI_PINCH_BEGIN) {
        s_zoom0 = s_zoom;
        set_status("捏合中…");
        return;
    }
    if (phase == QL_UI_PINCH_UPDATE) {
        apply_zoom((ql_coord_t)(((int64_t)s_zoom0 * scale_fx) / QL_SCALE_ONE));
        return;
    }
    set_status("捏合结束");
}

/* 每帧钩子：这里是唯一能拿到 gfx 的地方（缩放要它） */
static int on_frame(QLUIRoot *ui, QLGfxGL *gfx, void *user)
{
    (void)ui; (void)user;
    s_gfx = gfx;
    ql_gfx_gl_window_size(gfx, &s_win_w, &s_win_h);
    if (s_pending_zoom) {
        s_pending_zoom = 0;
        apply_zoom_now(s_zoom);   /* 值可能没变，但还没落到后端过 */
    }
    if (s_redraw) {
        s_redraw = 0;
        return 1;                 /* 缩放变了：整屏重绘 */
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (qlgl_init() != QL_OK)
        return 1;
    ex_setup_locale();
    ex_load_font();

    QLUIDoc *doc = ex_load_ui("index.html", "style.css");
    if (doc == NULL) {
        printf("界面编译失败：%s\n", ql_ui_text_last_error());
        qlgl_shutdown();
        return 1;
    }
    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        qlgl_shutdown();
        return 1;
    }
    s_ui = ui;

    /* 先布局一次：滚动范围（scroll_max_y）要等布局算出来才知道 */
    ql_ui_layout(ui, W, H);

    /*
     * 节点顺序 = 文档顺序（深度优先、父在前）：
     *   0 root · 1 .bar · 2 .title · 3 #zoom · 4 .body · 5 #pad · 6 .pad-main
     *   7 #cnt · 8 .hint · 9 #list · 10.. .row/.n … · 最后一个是 #status
     */
    if (ui->node_count < 10) {
        printf("界面节点太少（%d）\n", (int)ui->node_count);
        ql_ui_free(ui);
        ql_ui_doc_free(doc);
        qlgl_shutdown();
        return 1;
    }
    s_zoom_label = &ui->nodes[3];
    s_cnt = &ui->nodes[7];
    s_status = &ui->nodes[ui->node_count - 1];

    /* 滚动容器不按下标猜：整页只有一个 overflow-y: auto 的节点 */
    s_list = NULL;
    for (uint16_t i = 0; i < ui->node_count; i++)
        if (ql_ui_scrollable(&ui->nodes[i])) {
            s_list = &ui->nodes[i];
            break;
        }
    if (s_list == NULL) {
        printf("没找到可滚动的列表\n");
        ql_ui_free(ui);
        ql_ui_doc_free(doc);
        qlgl_shutdown();
        return 1;
    }

    ql_ui_set_click_cb(ui, on_click, NULL);
    ql_ui_set_pinch_cb(ui, on_pinch, NULL);
    ex_set_frame_cb(on_frame, NULL);

    /*
     * `--zoom 150`：启动时就按 150% 布局与绘制。截图对比用，省得手上没有触摸屏
     * 就没法验证缩放路径（缩放本身的识别仍然只在合成手势里验）。
     */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--zoom") == 0 && i + 1 < argc) {
            const long pct = strtol(argv[++i], NULL, 10);
            if (pct >= 50 && pct <= 300) {
                s_zoom = (ql_coord_t)((int64_t)pct * QL_SCALE_ONE / 100);
                s_pending_zoom = (s_zoom != QL_SCALE_ONE);
            }
        }
    }

    /*
     * 没有窗口时（离屏 / --dump）用合成事件走一遍触摸链路，验证手势识别：
     * 单指点按 → 1 次点击；单指拖列表 → 滚动且不算点击；双指拉开 → 放大。
     */
    if (argc == 1) {
        printf("合成手势（窗口模式请用触摸屏或鼠标）\n");
        /* 事件要经输入队列才会到界面层：先接上汇（ex_run 里会再接一次） */
        ql_ui_attach_input(ui);
        (void)ql_input_push_touch(60, 100, QL_TOUCH_DOWN, 0, 1);
        (void)ql_input_push_touch(60, 100, QL_TOUCH_UP, 0, 0);
        (void)ql_input_dispatch();

        (void)ql_input_push_touch(170, 400, QL_TOUCH_DOWN, 0, 1);
        for (int i = 1; i <= 6; i++)
            (void)ql_input_push_touch(170, 400 - i * 10, QL_TOUCH_MOVE, 0, 1);
        (void)ql_input_push_touch(170, 340, QL_TOUCH_UP, 0, 0);
        (void)ql_input_dispatch();
        printf("列表拖动后 scroll_y=%d（应 > 0），点击次数仍为 %d\n",
               (int)s_list->scroll_y, s_taps);

        (void)ql_input_push_touch(120, 400, QL_TOUCH_DOWN, 0, 1);
        (void)ql_input_push_touch(220, 400, QL_TOUCH_DOWN, 1, 2);
        (void)ql_input_push_touch(100, 400, QL_TOUCH_MOVE, 0, 2);
        (void)ql_input_push_touch(260, 400, QL_TOUCH_MOVE, 1, 2);
        (void)ql_input_push_touch(100, 400, QL_TOUCH_UP, 0, 1);
        (void)ql_input_push_touch(260, 400, QL_TOUCH_UP, 1, 0);
        (void)ql_input_dispatch();
        printf("捏合 100→160px：zoom=%d%%\n", (int)(s_zoom * 100 / QL_SCALE_ONE));
        ql_ui_detach_input();
    }

    const int rc = ex_run(ui, W, H, "QLGL · 37 touch", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
