# QLInput / 交互 API 手册（v0.1）

配套阅读：[QLInput 阅读指南](./QLInput阅读指南.md) · 界面与命中测试：[QLUI API手册](../ui/QLUI%20API%20手册.md)

- 头文件：`#include <qlgl/qlgl.h>`（已包含 `qlinput.h`）
- 链接：`target_link_libraries(xxx PRIVATE qlgl)`
- C11；队列静态分配，**运行期零 malloc**

---

## 1. 最小可运行示例（窗口 + 可点按钮）

```c
#include <qlgl/qlgl.h>

static const char *k_html =
    "<div id=\"root\"><div class=\"btn\">点我</div></div>";
static const char *k_css =
    "#root { width: 320px; height: 240px; background-color: #101820; }"
    ".btn  { position: absolute; left: 20px; top: 20px; width: 120px; height: 40px;"
    "        background-color: #2A3444; color: #E0E8F0; text-align: center;"
    "        line-height: 40px; }"
    ".btn:hover  { background-color: #3A506C; }"
    ".btn:active { background-color: #3CC07A; }";

/* 事件汇：把输入交给界面层。命中测试、:hover/:active、点击合成都由它做 */
static void sink(const QLInputEvent *ev, void *user)
{
    ql_ui_input_event((QLUIRoot *)user, ev);
}

static void on_click(QLUIRoot *ui, QLUINodeRT *node, void *user)
{
    (void)ui; (void)node; (void)user;
    printf("clicked!\n");
}

int main(void)
{
    qlgl_init();

    QLUIDoc *doc = ql_ui_load_text(k_html, k_css);
    QLUIRoot *ui = ql_ui_build(doc);
    ql_ui_layout(ui, 320, 240);
    ql_ui_set_click_cb(ui, on_click, NULL);
    ql_input_set_sink(sink, ui);          /* ① 装汇：事件 → QLUI */

    QLDispDriver *disp = ql_disp_mem_create(320, 240, QL_COLOR_RGB888);
    QLCanvas cv;
    ql_canvas_from_disp(&cv, disp);

    while (/* 你的主循环条件 */) {
        ql_input_dispatch();              /* ① 排空队列 → sink */
        ql_ui_render_canvas(ui, &cv);     /* ② 只重绘脏区 */
    }

    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    ql_disp_mem_destroy(disp);
    qlgl_shutdown();
    return 0;
}
```

主循环只有两步：**先派发输入，再重绘**。状态变化由界面层自己打脏区。

---

## 2. 数据结构和常量

### 2.1 `QLInputEvent`

```c
typedef struct {
    int32_t  x, y;        /* 指针逻辑坐标（窗口左上角为原点） */
    int32_t  dx, dy;      /* 滚轮增量；MOVE 时是相对位移 */
    uint32_t key;         /* QLKey（键盘事件） */
    uint32_t time_ms;     /* 时间戳，0 = 未知（双击判定用，M1 未用） */
    uint8_t  type;        /* QLInputType */
    uint8_t  button;      /* 本事件的按钮 QL_BUTTON_* */
    uint8_t  buttons;     /* 当前按住的按钮位图 QL_BUTTON_BIT_* */
    uint8_t  mods;        /* QL_MOD_* */
    char     text[8];     /* UTF-8 字符，type == QL_INPUT_TEXT 时有效 */
} QLInputEvent;
```

定长、可值传递、不含指针，因此可以直接放进环形队列、可以直接 memcpy。

### 2.2 `QLInputType`

| 值 | 含义 |
|---|---|
| `QL_INPUT_POINTER_MOVE` | 指针移动 |
| `QL_INPUT_POINTER_DOWN` | 按钮按下 |
| `QL_INPUT_POINTER_UP` | 按钮抬起 |
| `QL_INPUT_POINTER_LEAVE` | 指针离开窗口表面 |
| `QL_INPUT_WHEEL` | 滚轮 / 触控板滚动 |
| `QL_INPUT_KEY_DOWN` / `QL_INPUT_KEY_UP` | 键盘 |
| `QL_INPUT_TEXT` | 已翻译好的 UTF-8 字符 |

### 2.3 `QLKey`

可打印字符**直接用 ASCII**，特殊键从 `0x100` 起：

| 值 | 键 |
|---|---|
| `8 / 9 / 13 / 27 / 32 / 127` | Backspace / Tab / Enter / Escape / Space / Delete |
| `'a'..'z'`、`'0'..'9'`、标点 | 同 ASCII |
| `0x100..0x103` | Left / Up / Right / Down |
| `0x104..0x109` | Home / End / PageUp / PageDown / Insert |
| `0x10A..0x115` | F1..F12 |

判断可打印：`key < 0x80`。

### 2.4 修饰键与按钮

```c
QL_MOD_SHIFT / QL_MOD_CTRL / QL_MOD_ALT / QL_MOD_META / QL_MOD_CAPS   /* 位图 */
QL_BUTTON_LEFT = 0, QL_BUTTON_MIDDLE = 1, QL_BUTTON_RIGHT = 2          /* 索引 */
QL_BUTTON_BIT_LEFT = 1, QL_BUTTON_BIT_MIDDLE = 2, QL_BUTTON_BIT_RIGHT = 4  /* 位图 */
```

## 3. 队列与派发

```c
void      ql_input_reset(void);
ql_err_t  ql_input_push(const QLInputEvent *ev);
uint32_t  ql_input_pending(void);
int       ql_input_dispatch(void);
void      ql_input_handle(const QLInputEvent *ev);   /* 立即派发单条 */
```

| 函数 | 说明 |
|---|---|
| `ql_input_reset` | 清空队列 + 复位 hover/pressed/focus + 清 sink。`qlgl_init/shutdown` 会调用 |
| `ql_input_push` | 入队。队列满（`QL_CFG_INPUT_QUEUE`，默认 64）时**丢最旧的**并返回 `QL_ERR_FULL` |
| `ql_input_dispatch` | 排空队列，返回处理条数。会调用户回调，回调里可继续 push |
| `ql_input_handle` | 绕过队列立即派发，用于"已经在事件回调里、只想立刻生效"的场景 |

### 构造 + 入队的便捷函数（后端与测试都用）

```c
ql_err_t ql_input_push_move(ql_coord_t x, ql_coord_t y, uint8_t buttons);
ql_err_t ql_input_push_button(ql_coord_t x, ql_coord_t y, uint8_t button, int down);
ql_err_t ql_input_push_leave(void);
ql_err_t ql_input_push_wheel(ql_coord_t x, ql_coord_t y, int32_t dx, int32_t dy);
ql_err_t ql_input_push_key(uint32_t key, int down, uint8_t mods);
ql_err_t ql_input_push_text(const char *utf8);
```

> 这些函数**只入队不派发**。要在测试里立刻看到效果，记得跟一句 `ql_input_dispatch()`。

---

## 4. 状态查询

```c
uint8_t    ql_input_buttons(void);   /* 当前按住的按钮位图 */
uint8_t    ql_input_mods(void);      /* 最近一次键盘事件的修饰键 */
ql_coord_t ql_input_x(void);         /* 最近一次指针位置 */
ql_coord_t ql_input_y(void);
```

这四个值在**入队时**就更新（`ql_input_push*` 内部跟踪），所以任何时候读到的
都是"已入队事件"的结果，不会拿到过期快照。

**命中测试不在这里。** 命中谁、谁进入 `:hover`、点击落在哪个节点上，
全都是界面层的事：

```c
QLUINodeRT *ql_ui_hit_test(QLUIRoot *ui, ql_coord_t x, ql_coord_t y);
int         ql_ui_pointer_down(QLUIRoot *ui, ql_coord_t x, ql_coord_t y);
```

为什么分开放：只有界面层认识 HTML/CSS 编译出来的节点树（`display:none`、
`overflow` 裁剪、`transform`、热区……），输入层不该知道这些。

---

## 5. 事件汇（sink）：把输入交给界面层

```c
typedef void (*ql_input_sink)(const QLInputEvent *ev, void *user);
void ql_input_set_sink(ql_input_sink sink, void *user);
```

`ql_input_dispatch()` **只认装好的汇**：不装汇时事件被丢弃（队列照常排空）。
没有"默认派发目标"这回事 —— 这一层不知道界面长什么样。

界面是 `QLUIRoot`（HTML/CSS 编译出来的）时，最省事的装法是：

```c
ql_ui_attach_input(ui);
while (running) {
    running = ql_disp_x11_poll(disp);
    ql_input_dispatch();      /* 指针事件 → QLUI 的 :hover/:active + click_cb */
    ql_ui_render_canvas(ui, &canvas);
}
```

需要自己再处理按键（Esc 退出等）时，装自己的 sink：

```c
static void sink(const QLInputEvent *ev, void *user)
{
    if (ev->type == QL_INPUT_KEY_DOWN && ev->key == QL_KEY_ESCAPE) {
        ql_disp_x11_close(disp);
        return;
    }
    ql_ui_input_event((QLUIRoot *)user, ev);
}
ql_input_set_sink(sink, ui);
```

---

## 6. HTML + CSS 里的 `:hover / :active / :focus`

这三个伪类在**构建期**被编译成独立样式表（运行期只切一个下标）：

```css
.btn:hover  { background-color: #4ad08a; border-color: #e0ffe0; }
.btn:active { background-color: #2a9a5c; }
.btn:focus  { border-color: #ffd060; }
```

运行期接口：

```c
int         ql_ui_pointer_down(QLUIRoot *ui, ql_coord_t x, ql_coord_t y);
int         ql_ui_pointer_up(QLUIRoot *ui, ql_coord_t x, ql_coord_t y);
int         ql_ui_pointer_move(QLUIRoot *ui, ql_coord_t x, ql_coord_t y);
void        ql_ui_set_focus_node(QLUIRoot *ui, QLUINodeRT *node);
QLUINodeRT *ql_ui_focus_node(const QLUIRoot *ui);
const QLUIStyle *ql_ui_node_style(const QLUINodeRT *node);  /* 生效样式 */
```

要点：

- `ql_ui_pointer_move` 返回 1 表示**悬停目标变了**（该重绘），返回 0 表示没变；
- 状态位沿祖先链生效（悬停子元素时父容器也算 `:hover`）；
- 状态样式**只影响绘制，不影响布局**：布局永远按基础样式算，
  所以 `:hover` 里改 `padding` 不会导致重排（M1 的限制，已文档化）；
- 只支持单个伪类；`:hover:active` 这种组合会**构建期报错**，不会被静默忽略。

---

## 7. 后端接口（给写后端的人）

### X11

```c
void ql_disp_x11_close(QLDispDriver *driver);   /* 从按键回调里请求关闭窗口 */
```

事件掩码必须包含 `ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
EnterWindowMask | LeaveWindowMask | KeyPressMask | KeyReleaseMask`。
翻译逻辑复用 `src/port/qlinput_x11.h`（内部头文件）。

### Wayland

```c
void ql_disp_wayland_close(QLDispDriver *driver);
```

### GL（X11 窗口）

```c
void ql_gfx_gl_close(QLGfxGL *gfx);
```

### 后端约定（三条）

1. **只 push，不派发**；
2. 坐标用**逻辑坐标**（窗口左上角原点）；
3. 滚轮统一成 `dy > 0 = 向下`，X11 的 `Button4`（向上）要变成 `dy = -1`。

---

## 8. 容量与调参

| 宏 | 默认 | 说明 |
|---|---|---|
| `QL_CFG_INPUT_QUEUE` | 64 | 输入事件环形队列深度（静态，`sizeof(QLInputEvent)*N` ≈ 36N 字节） |

队列在 64 深度下占约 2.3 KB 静态内存，MCU 上可以调到 8~16。

---

## 9. 常见问题

**Q：点了按钮没反应**
1. 主循环里有没有 `ql_input_dispatch()`？
2. 装汇了吗（`ql_input_set_sink` 或 `ql_ui_attach_input`）？
3. 后端有没有真的 push 事件（`ql_input_pending()` 是否 > 0）？
4. 元素是不是 `display: none`，或被祖先的 `overflow: hidden` 裁掉了？
   用 `ql_ui_hit_test()` 直接问"这个点命中了谁"。

**Q：`:hover` 生效了但点了没反应**
点击要求"按下与抬起命中同一个节点"。`ql_ui_set_click_cb()` 之外还要确认
节点不是 `<img>`（图片上的点击走热区，见 `ql_ui_set_area_cb`）。

**Q：键盘事件没人收到**
界面层只在有焦点时处理按键。先点一下需要键盘的控件（`<input>` / `<select>`），
它会自己拿焦点；也可以 `ql_ui_set_focus_node()` 主动设置。

**Q：Wayland 上按字母没反应**
evdev → US 布局静态表，非 US 布局或其他输入法要接 xkbcommon（未做）。
