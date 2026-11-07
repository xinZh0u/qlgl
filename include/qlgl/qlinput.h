/**
 * @file    qlinput.h
 * @brief   输入系统：平台无关的输入事件队列 + 命中测试 + hover/focus/click 合成。
 *
 * 分工（和绘制一样严格分层）：
 *
 *     后端(X11/Wayland/mem)  ──push──▶  QLInputEvent 队列
 *                                          │
 * 主循环每帧：ql_input_dispatch() ──────────┘
 *                │
 *                ├─ 命中测试 → 目标控件
 *                ├─ 更新 hover/active/focus 状态位（并打脏）
 *                └─ ql_emit() → 捕获/目标/冒泡（复用事件系统）
 *
 * 后端**不**知道控件树，只负责把"坐标 + 按钮 + 键码"翻译成 QLInputEvent。
 * 因此同一套交互逻辑在窗口、离屏、单元测试里完全一致。
 */
#ifndef QLGL_INPUT_H
#define QLGL_INPUT_H

#include "qlgl.h"

typedef struct QLObject QLObject;

/* ----- 事件类型 ----- */
typedef enum {
    QL_INPUT_NONE = 0,
    QL_INPUT_POINTER_MOVE,      /* 指针移动 */
    QL_INPUT_POINTER_DOWN,      /* 按钮按下 */
    QL_INPUT_POINTER_UP,        /* 按钮抬起 */
    QL_INPUT_POINTER_LEAVE,     /* 指针离开窗口表面 */
    QL_INPUT_WHEEL,             /* 滚轮/触控板滚动 */
    QL_INPUT_KEY_DOWN,
    QL_INPUT_KEY_UP,
    QL_INPUT_TEXT,              /* 已翻译好的 UTF-8 字符输入 */
    QL_INPUT_TYPE_COUNT
} QLInputType;

/* ----- 逻辑键码 -----
 * 可打印字符直接用 ASCII（'a'..'z'、'0'..'9'、标点），
 * 特殊键从 0x100 起，避免与 ASCII 冲突。
 */
typedef enum {
    QL_KEY_NONE        = 0,
    QL_KEY_BACKSPACE   = 8,
    QL_KEY_TAB         = 9,
    QL_KEY_ENTER       = 13,
    QL_KEY_ESCAPE      = 27,
    QL_KEY_SPACE       = 32,
    QL_KEY_DELETE      = 127,

    QL_KEY_LEFT        = 0x100,
    QL_KEY_UP,
    QL_KEY_RIGHT,
    QL_KEY_DOWN,
    QL_KEY_HOME,
    QL_KEY_END,
    QL_KEY_PAGE_UP,
    QL_KEY_PAGE_DOWN,
    QL_KEY_INSERT,
    QL_KEY_F1,
    QL_KEY_F2,
    QL_KEY_F3,
    QL_KEY_F4,
    QL_KEY_F5,
    QL_KEY_F6,
    QL_KEY_F7,
    QL_KEY_F8,
    QL_KEY_F9,
    QL_KEY_F10,
    QL_KEY_F11,
    QL_KEY_F12,
} QLKey;

/* ----- 修饰键位图 ----- */
enum {
    QL_MOD_SHIFT = 1u << 0,
    QL_MOD_CTRL  = 1u << 1,
    QL_MOD_ALT   = 1u << 2,
    QL_MOD_META  = 1u << 3,
    QL_MOD_CAPS  = 1u << 4,
};

/* ----- 鼠标按钮 ----- */
enum {
    QL_BUTTON_LEFT   = 0,
    QL_BUTTON_MIDDLE = 1,
    QL_BUTTON_RIGHT  = 2,
};

/* 按钮位图（buttons 字段用） */
enum {
    QL_BUTTON_BIT_LEFT   = 1u << 0,
    QL_BUTTON_BIT_MIDDLE = 1u << 1,
    QL_BUTTON_BIT_RIGHT  = 1u << 2,
};

/* ----- 原始输入事件：定长、可值传递、不含指针 ----- */
typedef struct {
    int32_t  x, y;        /* 指针逻辑坐标（窗口左上角为原点） */
    int32_t  dx, dy;      /* 滚轮增量；MOVE 时是相对位移 */
    uint32_t key;         /* QLKey（键盘事件） */
    uint32_t time_ms;     /* 事件时间戳，0 表示未知（用于双击判定，M1 未用） */
    uint8_t  type;        /* QLInputType */
    uint8_t  button;      /* 本次事件涉及的按钮（QL_BUTTON_*） */
    uint8_t  buttons;     /* 当前按住的按钮位图 */
    uint8_t  mods;        /* QL_MOD_* */
    char     text[8];     /* UTF-8 字符，type == QL_INPUT_TEXT 时有效 */
} QLInputEvent;

/* ----- 队列 ----- */
void        ql_input_reset(void);                 /* 清空队列并复位 hover/pressed/focus */
ql_err_t    ql_input_push(const QLInputEvent *ev);/* 入队；满则丢最旧的并返回 QL_ERR_FULL */
uint32_t    ql_input_pending(void);               /* 队列中待派发的事件数 */
int         ql_input_dispatch(void);              /* 排空队列，返回实际派发的事件数 */
void        ql_input_handle(const QLInputEvent *ev); /* 立即派发单条（不经过队列） */

/* 构造 + 入队的便捷函数：后端用它，测试也用它 */
ql_err_t    ql_input_push_move(ql_coord_t x, ql_coord_t y, uint8_t buttons);
ql_err_t    ql_input_push_button(ql_coord_t x, ql_coord_t y, uint8_t button, int down);
ql_err_t    ql_input_push_leave(void);
ql_err_t    ql_input_push_wheel(ql_coord_t x, ql_coord_t y, int32_t dx, int32_t dy);
ql_err_t    ql_input_push_key(uint32_t key, int down, uint8_t mods);
ql_err_t    ql_input_push_text(const char *utf8);

/* ----- 事件根 -----
 * 未显式设置时回落到 ql_render_root()，因此绝大多数程序不需要调用 set_root。
 */
void        ql_input_set_root(QLObject *root);
QLObject   *ql_input_root(void);

/* ----- 状态查询 ----- */
QLObject *ql_input_hover(void);       /* 当前悬停目标 */
QLObject *ql_input_pressed(void);     /* 当前按下目标（按下到抬起之间保持） */
QLObject *ql_input_focus(void);       /* 当前键盘焦点 */
ql_err_t  ql_input_set_focus(QLObject *obj);   /* NULL 表示清除焦点 */
uint8_t   ql_input_buttons(void);     /* 当前按住的按钮位图 */
uint8_t   ql_input_mods(void);        /* 最近一次键盘事件的修饰键 */
ql_coord_t ql_input_x(void);          /* 最近一次指针位置 */
ql_coord_t ql_input_y(void);

/* ----- 命中测试 -----
 * 返回该点最上层的可交互控件（按绘制顺序取最后一个命中的），没有则返回 NULL。
 * root 为 NULL 时使用 ql_input_root()。
 */
QLObject *ql_input_hit_test(QLObject *root, ql_coord_t x, ql_coord_t y);

/* ----- 事件汇（可选） -----
 * 默认 ql_input_dispatch() 把事件派发给 QLWidget 树。
 * 如果界面是 QLUIRoot（HTML/CSS 编译出来的），用 ql_ui_attach_input()
 * 装一个汇，事件就改由那个模型处理，主循环仍然只调 ql_input_dispatch()。
 */
typedef void (*ql_input_sink)(const QLInputEvent *ev, void *user);
void ql_input_set_sink(ql_input_sink sink, void *user);

#endif /* QLGL_INPUT_H */
