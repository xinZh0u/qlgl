/**
 * @file    example_input.c
 * @brief   交互示例：可悬停 / 按下 / 聚焦的按钮，键盘方向键移动焦点。
 *
 * 演示的内容：
 *   - QLWidget 状态背景（QL_WIDGET_STATE_HOVER / ACTIVE / FOCUS）
 *   - 事件冒泡到容器
 *   - 命中测试（重叠控件取最上层）
 *   - 焦点与键盘事件
 *
 * 运行：
 *   ./build/example_input                 打开窗口，鼠标点按钮，Esc 退出
 *   ./build/example_input --frames 120    跑 120 帧后退出
 *   ./build/example_input --mem --ppm a.ppm
 *   ./build/example_input --demo          无窗口自动化演示（注入事件，打印状态）
 */
#include <stdio.h>
#include <string.h>

#include <qlgl/qlgl.h>

#include "../window_helper.h"

#if defined(__linux__)
#include <time.h>
extern int nanosleep(const struct timespec *req, struct timespec *rem);
#endif

#define WIN_W 360
#define WIN_H 260
#define BTN_W 96
#define BTN_H 34

static QLWidget *s_buttons[3];
static QLLabel  *s_status;
static int       s_clicks;
static int       s_focus_index;

static void refresh_status(void)
{
    char buf[64];
    const char *name[3] = { "Alpha", "Beta", "Gamma" };
    snprintf(buf, sizeof(buf), "clicks %d / focus %s", s_clicks,
             s_focus_index >= 0 ? name[s_focus_index] : "-");
    ql_label_set_text((QLObject *)s_status, buf);
}

static void on_click(QLEvent *ev)
{
    QL_UNUSED(ev);
    s_clicks++;
    refresh_status();
    printf("click #%d\n", s_clicks);
}

/* 焦点切换：清掉所有按钮的 FOCUS 状态，再设新的 */
static void set_focus_index(int index)
{
    if (index < 0)
        index = 2;
    if (index > 2)
        index = 0;
    s_focus_index = index;
    for (int i = 0; i < 3; i++)
        ql_widget_set_state((QLObject *)s_buttons[i], QL_WIDGET_STATE_FOCUS, i == index);
    ql_input_set_focus((QLObject *)s_buttons[index]);
    refresh_status();
}

static void on_key(QLEvent *ev)
{
    int32_t key = ql_event_arg_int(ev, 0, 0);
    if (key == QL_KEY_LEFT)
        set_focus_index(s_focus_index - 1);
    else if (key == QL_KEY_RIGHT)
        set_focus_index(s_focus_index + 1);
    else if (key == QL_KEY_ENTER && s_focus_index >= 0)
        QL_EMIT_0((QLObject *)s_buttons[s_focus_index], QL_WIDGET_EVENT_CLICK);
    else
        return;
    ql_event_stop_propagation(ev);      /* 已处理，不再冒泡 */
}

static QLWidget *make_button(QLObject *parent, const char *text, int index,
                             ql_coord_t x, ql_coord_t y)
{
    QLWidget *btn = ql_widget_new_child(parent, x, y, BTN_W, BTN_H);
    ql_widget_set_bg((QLObject *)btn, QL_COLOR_RGB(0x2A, 0x34, 0x44), 255);
    ql_widget_set_border((QLObject *)btn, 2, QL_COLOR_RGB(0x50, 0x60, 0x78), 10);
    ql_widget_set_state_bg((QLObject *)btn, QL_WIDGET_STATE_HOVER,
                           QL_COLOR_RGB(0x3A, 0x50, 0x6C), 255);
    ql_widget_set_state_bg((QLObject *)btn, QL_WIDGET_STATE_ACTIVE,
                           QL_COLOR_RGB(0x3C, 0xC0, 0x7A), 255);
    ql_widget_set_state_bg((QLObject *)btn, QL_WIDGET_STATE_FOCUS,
                           QL_COLOR_RGB(0x33, 0x4A, 0x66), 255);
    ql_widget_set_focusable((QLObject *)btn, 1);

    QLLabel *label = ql_label_new_child((QLObject *)btn, text, 0, 0);
    ql_label_set_font((QLObject *)label, &ql_font_mono_12);
    ql_label_set_text_color((QLObject *)label, QL_COLOR_RGB(0xD0, 0xD8, 0xE4));
    ql_label_set_align((QLObject *)label, QL_TEXT_ALIGN_CENTER);
    /* 让标签铺满按钮，文本靠按钮的居中对齐生效 */
    ql_object_set_property((QLObject *)label, QL_WIDGET_PROPERTY_X, ql_value_int(0));
    ql_object_set_property((QLObject *)label, QL_WIDGET_PROPERTY_Y, ql_value_int(9));
    ql_object_set_property((QLObject *)label, QL_WIDGET_PROPERTY_WIDTH,
                           ql_value_int(BTN_W));
    ql_object_set_property((QLObject *)label, QL_WIDGET_PROPERTY_HEIGHT,
                           ql_value_int(16));
    /* 标签不该抢走按钮的命中/悬停 */
    ql_widget_set_pointer_events((QLObject *)label, 0);

    ql_connect((QLObject *)btn, QL_WIDGET_EVENT_CLICK, on_click, NULL);
    s_buttons[index] = btn;
    return btn;
}

int main(int argc, char **argv)
{
    QLWin win;
    ql_win_parse_args(&win, argc, argv);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--demo") == 0)
            win.force_mem = 1;
    }

    if (qlgl_init() != QL_OK) {
        printf("qlgl_init 失败\n");
        return 1;
    }
    ql_win_load_font();
    if (!ql_win_open(&win, "QLGL Input Demo", WIN_W, WIN_H, QL_COLOR_FORMAT_AUTO)) {
        qlgl_shutdown();
        return 1;
    }
    ql_disp_set_bg_color(QL_COLOR_RGB(0x10, 0x14, 0x1C));

    QLWidget *root = ql_widget_new_ex(0, 0, WIN_W, WIN_H);
    ql_widget_set_bg((QLObject *)root, QL_COLOR_RGB(0x10, 0x14, 0x1C), 255);
    ql_render_set_root((QLObject *)root);
    ql_win_bind_esc(&win, (QLObject *)root);

    QLWidget *card = ql_widget_new_child((QLObject *)root, 16, 16, WIN_W - 32, WIN_H - 32);
    ql_widget_set_bg((QLObject *)card, QL_COLOR_RGB(0x1A, 0x21, 0x2C), 255);
    ql_widget_set_border((QLObject *)card, 2, QL_COLOR_RGB(0x33, 0x3F, 0x52), 14);

    QLLabel *title = ql_label_new_child((QLObject *)card, "QLGL Input", 16, 14);
    ql_label_set_font((QLObject *)title, &ql_font_mono_16);
    ql_label_set_text_color((QLObject *)title, QL_COLOR_RGB(0x33, 0x99, 0xFF));

    make_button((QLObject *)card, "Alpha", 0, 16, 52);
    make_button((QLObject *)card, "Beta", 1, 16 + BTN_W + 8, 52);
    make_button((QLObject *)card, "Gamma", 2, 16 + 2 * (BTN_W + 8), 52);

    s_status = ql_label_new_child((QLObject *)card, "", 16, 100);
    ql_label_set_font((QLObject *)s_status, &ql_font_mono_12);
    ql_label_set_text_color((QLObject *)s_status, QL_COLOR_RGB(0xF0, 0xB0, 0x30));

    /* 键盘事件冒泡到这里（焦点在按钮上也能收到，因为冒泡到根） */
    ql_connect((QLObject *)root, QL_WIDGET_EVENT_KEY_DOWN, on_key, NULL);
    set_focus_index(0);

    if (win.force_mem && win.max_frames <= 1) {
        /* 无窗口自动化演示：直接往队列里注入事件，验证整条链路 */
        printf("-- demo: 注入 hover / click / key 事件\n");
        ql_input_push_move(40, 80, 0);
        ql_input_dispatch();
        printf("  hover -> %s\n", ql_input_hover() == (QLObject *)s_buttons[0]
                                        ? "按钮 0（命中正确）" : "未命中");
        ql_input_push_button(40, 80, QL_BUTTON_LEFT, 1);
        ql_input_push_button(40, 80, QL_BUTTON_LEFT, 0);
        ql_input_dispatch();
        printf("  click -> %d 次\n", s_clicks);
        ql_input_push_key(QL_KEY_RIGHT, 1, 0);
        ql_input_dispatch();
        printf("  key   -> 焦点 %d\n", s_focus_index);
    }

    long frame = 0;
    while (ql_win_poll(&win)) {
        ql_input_dispatch();
        if (ql_render() < 0)
            break;

        frame++;
        if (win.max_frames > 0 && frame >= win.max_frames)
            break;
        if (win.kind != QL_WIN_KIND_MEM) {
            struct timespec ts = { 0, 16000000L };
            nanosleep(&ts, NULL);
        }
    }

    ql_win_close(&win);
    ql_object_unref((QLObject *)root);

    if (win.force_mem) {
        QLMemStats st;
        ql_mem_stats(&st);
        printf("最终：clicks=%d focus=%d，未释放 %zu 字节\n",
               s_clicks, s_focus_index, st.bytes_live);
    }

    qlgl_shutdown();
    return 0;
}
