/**
 * @file    test_x11_text.c
 * @brief   X11 键事件 → 输入事件：**打字必须有 QL_INPUT_TEXT**。
 *
 * 这里守的是 Linux 上"键盘打不了字"那个 bug：`QLKey` 只表达"哪个键"，
 * 界面层插入字符走的是 `QL_INPUT_TEXT`，而翻译层原来只发前者。
 * 表现很迷惑：方向键能移光标、退格能删字，但打一个字都进不去。
 *
 * 不需要开窗口：直接合成 XKeyEvent 喂给翻译层，看它往队列里放了什么。
 * 没有 X 显示时整条测试跳过（打印一句说明，不算失败）。
 */
#include <stdio.h>
#include <string.h>

#include "../ql_test.h"

#if defined(QLGL_ENABLE_X11) && QLGL_ENABLE_X11
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <qlgl/qlgl.h>

#include "../../src/port/qlinput_x11.h"

static int    s_ntext;
static int    s_nkey;
static char   s_text[32];
static uint32_t s_last_key;

static void sink(const QLInputEvent *ev, void *user)
{
    (void)user;
    if (ev->type == QL_INPUT_TEXT) {
        if (s_ntext == 0)
            snprintf(s_text, sizeof(s_text), "%s", ev->text);
        s_ntext++;
    } else if (ev->type == QL_INPUT_KEY_DOWN) {
        s_nkey++;
        s_last_key = ev->key;
    }
}

static void feed(Display *d, KeySym sym, unsigned state)
{
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = KeyPress;
    ev.xkey.display = d;
    ev.xkey.keycode = XKeysymToKeycode(d, sym);
    ev.xkey.state = state;
    s_ntext = 0;
    s_nkey = 0;
    s_text[0] = '\0';
    uint8_t buttons = 0;
    ql_x11_push_input_scaled(&ev, &buttons, QL_SCALE_ONE);
    (void)ql_input_dispatch();
}
#endif

QL_TEST_BEGIN("test_x11_text")

#if defined(QLGL_ENABLE_X11) && QLGL_ENABLE_X11
    Display *d = XOpenDisplay(NULL);
    if (d == NULL) {
        printf("    没有 X 显示，跳过（这条测不了 X11 键位翻译）\n");
    } else {
        qlgl_init();
        ql_input_reset();
        ql_input_set_sink(sink, NULL);

        /* 小写字母 */
        feed(d, XK_x, 0);
        QL_CHECK_EQ(s_ntext, 1);
        QL_CHECK(strcmp(s_text, "x") == 0);

        /* 大写（Shift） */
        feed(d, XK_A, ShiftMask);
        QL_CHECK_EQ(s_ntext, 1);
        QL_CHECK(strcmp(s_text, "A") == 0);

        /* 空格：既是文本也要有 QL_KEY_SPACE */
        feed(d, XK_space, 0);
        QL_CHECK_EQ(s_ntext, 1);
        QL_CHECK(strcmp(s_text, " ") == 0);
        QL_CHECK_EQ((int)s_last_key, (int)QL_KEY_SPACE);

        /* 数字 */
        feed(d, XK_4, 0);
        QL_CHECK_EQ(s_ntext, 1);
        QL_CHECK(strcmp(s_text, "4") == 0);

        /* 方向键：不产生文本，但要产生按键 */
        feed(d, XK_Left, 0);
        QL_CHECK_EQ(s_ntext, 0);
        QL_CHECK_EQ((int)s_last_key, (int)QL_KEY_LEFT);

        /* Ctrl+A / Alt+A：快捷键，不该插入字符 */
        feed(d, XK_a, ControlMask);
        QL_CHECK_EQ(s_ntext, 0);
        feed(d, XK_a, Mod1Mask);
        QL_CHECK_EQ(s_ntext, 0);

        /* 回车：不产生文本（界面层自己决定插不插换行） */
        feed(d, XK_Return, 0);
        QL_CHECK_EQ(s_ntext, 0);
        QL_CHECK_EQ((int)s_last_key, (int)QL_KEY_ENTER);

        ql_input_set_sink(NULL, NULL);
        XCloseDisplay(d);
        qlgl_shutdown();
    }
#else
    printf("    未启用 X11，跳过\n");
#endif

QL_TEST_END()
