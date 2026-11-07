# QLInput / 交互 API 手册（v0.1）

配套阅读：[QLInput 阅读指南](./QLInput阅读指南.md) · 对象/事件：[QLObject API手册](../object/QLObject%20API手册.md) · 绘制：[QLWidget API手册](../widget/QLWidget%20API手册.md)

- 头文件：`#include <qlgl/qlgl.h>`（已包含 `qlinput.h`）
- 链接：`target_link_libraries(xxx PRIVATE qlgl)`
- C11；队列静态分配，**运行期零 malloc**

---

## 1. 最小可运行示例（窗口 + 可点按钮）

```c
#include <qlgl/qlgl.h>

static void on_click(QLEvent *ev) { printf("clicked!\n"); }

int main(void)
{
    qlgl_init();
    QLDispDriver *disp = ql_disp_x11_create("Demo", 320, 240, QL_COLOR_FORMAT_AUTO);
    if (disp == NULL)
        disp = ql_disp_mem_create(320, 240, QL_COLOR_RGB888);

    QLWidget *root = ql_widget_new_ex(0, 0, 320, 240);
    ql_render_set_root((QLObject *)root);          /* 输入根默认回落到渲染根 */

    QLWidget *btn = ql_widget_new_child((QLObject *)root, 20, 20, 120, 40);
    ql_widget_set_bg((QLObject *)btn, QL_COLOR_RGB(0x2A, 0x34, 0x44), 255);
    ql_widget_set_state_bg((QLObject *)btn, QL_WIDGET_STATE_HOVER,
                           QL_COLOR_RGB(0x3A, 0x50, 0x6C), 255);
    ql_widget_set_state_bg((QLObject *)btn, QL_WIDGET_STATE_ACTIVE,
                           QL_COLOR_RGB(0x3C, 0xC0, 0x7A), 255);
    ql_connect((QLObject *)btn, QL_WIDGET_EVENT_CLICK, on_click, NULL);

    while (ql_disp_x11_poll(disp)) {
        ql_input_dispatch();      /* ① 处理输入 */
        ql_render();              /* ② 绘制脏区 */
    }

    ql_disp_x11_destroy(disp);
    ql_object_unref((QLObject *)root);
    qlgl_shutdown();
    return 0;
}
```

主循环只有两步：**先派发输入，再重绘**。输入层已经把"哪个控件变了状态"打成脏区。

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

### 2.5 控件状态位

```c
QL_WIDGET_STATE_HOVER / QL_WIDGET_STATE_ACTIVE / QL_WIDGET_STATE_FOCUS
```

三者可同时成立，由输入层沿**父链**设置（等价 CSS 的 `:hover` 对祖先生效）。

---

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

## 4. 根、状态查询、命中测试

```c
void      ql_input_set_root(QLObject *root);
QLObject *ql_input_root(void);

QLObject *ql_input_hover(void);
QLObject *ql_input_pressed(void);
QLObject *ql_input_focus(void);
ql_err_t  ql_input_set_focus(QLObject *obj);   /* NULL = 清除焦点 */
uint8_t   ql_input_buttons(void);
uint8_t   ql_input_mods(void);
ql_coord_t ql_input_x(void);
ql_coord_t ql_input_y(void);

QLObject *ql_input_hit_test(QLObject *root, ql_coord_t x, ql_coord_t y);
```

- `ql_input_root()` 未显式设置时**回落到 `ql_render_root()`**，所以大多数程序不用调 `set_root`；
- `ql_input_hit_test(NULL, x, y)` 等价于从输入根开始测；
- `ql_input_set_focus` 会发 `FOCUS_OUT` / `FOCUS_IN` 并同步状态位；传非控件对象返回 `QL_ERR_INVAL`。

---

## 5. 控件事件

事件 id 在 `QL_WIDGET_EVENT_*`（`qlwidget.h`），**参数顺序是契约**：

| 事件 | 参数 | 触发时机 |
|---|---|---|
| `QL_WIDGET_EVENT_POINTER_ENTER` | `(x, y)` | 指针进入该控件（换成它成为悬停目标时） |
| `QL_WIDGET_EVENT_POINTER_LEAVE` | `(x, y)` | 不再是悬停目标 |
| `QL_WIDGET_EVENT_POINTER_MOVE` | `(x, y, buttons)` | 指针移动（按住拖动时仍发给按下目标） |
| `QL_WIDGET_EVENT_POINTER_DOWN` | `(x, y, button)` | 按钮按下 |
| `QL_WIDGET_EVENT_POINTER_UP` | `(x, y, button)` | 按钮抬起 |
| `QL_WIDGET_EVENT_CLICK` | `(x, y, button)` | 同目标按下并抬起，位移 ≤ 4px |
| `QL_WIDGET_EVENT_WHEEL` | `(x, y, dy)` | 滚轮；`dy` 正 = 向下 |
| `QL_WIDGET_EVENT_KEY_DOWN` | `(key, mods)` | 键盘按下（目标是焦点控件） |
| `QL_WIDGET_EVENT_KEY_UP` | `(key, mods)` | 键盘抬起 |
| `QL_WIDGET_EVENT_TEXT` | `(utf8)` | 字符输入（目标是焦点控件） |
| `QL_WIDGET_EVENT_FOCUS_IN` / `FOCUS_OUT` | 无 | 焦点切换 |

读参数：

```c
static void on_down(QLEvent *ev)
{
    int x = ql_event_arg_int(ev, 0, 0);
    int y = ql_event_arg_int(ev, 1, 0);
    int button = ql_event_arg_int(ev, 2, QL_BUTTON_LEFT);
    ql_event_stop_propagation(ev);          /* 不想让父控件继续处理 */
}
```

**全部事件默认可冒泡、可 `stop_propagation`。** 冒泡时 `ev->target` 仍是原始目标，
`ev->current_target` 是当前处理者 —— 可以用它做"子控件没处理，容器兜底"：

```c
if (ev->target == ev->current_target)
    return;                                 /* 子控件自己已经处理过了 */
```

---

## 6. 控件状态 API

```c
uint8_t ql_widget_state(QLObject *obj);
int     ql_widget_has_state(QLObject *obj, uint8_t mask);
int     ql_widget_set_state(QLObject *obj, uint8_t mask, int on);  /* 返回 1 = 确实变了 */

int     ql_widget_pointer_events(QLObject *obj);
void    ql_widget_set_pointer_events(QLObject *obj, int enabled);
int     ql_widget_is_focusable(QLObject *obj);
void    ql_widget_set_focusable(QLObject *obj, int focusable);

void    ql_widget_set_state_bg(QLObject *obj, uint8_t state, ql_color_t color, uint8_t alpha);
```

`ql_widget_set_state_bg` 是**最小可用的状态样式**：某个状态成立时用这套颜色替代
`bg_color/bg_alpha`。优先级 **ACTIVE > FOCUS > HOVER**；`alpha == 0` 表示该状态不改变外观。
`draw_default` 里生效，自定义绘制函数想用就自己查 `ql_widget_state()`。

> 完整的状态样式（边框、圆角、文字色）走 HTML/CSS 路线，见 §8。

---

## 7. 事件汇（sink）：把输入交给别的模型

```c
typedef void (*ql_input_sink)(const QLInputEvent *ev, void *user);
void ql_input_set_sink(ql_input_sink sink, void *user);
```

默认 `ql_input_dispatch()` 把事件派发给 `QLWidget` 树。
如果界面是 `QLUIRoot`（HTML/CSS 编译出来的），装一个 sink 就能共用同一条主循环：

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

## 8. HTML + CSS 里的 `:hover / :active / :focus`

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

## 9. 后端接口（给写后端的人）

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

## 10. 容量与调参

| 宏 | 默认 | 说明 |
|---|---|---|
| `QL_CFG_INPUT_QUEUE` | 64 | 输入事件环形队列深度（静态，`sizeof(QLInputEvent)*N` ≈ 36N 字节） |
| `QL_CLICK_SLOP` | 4 px | 内部常量，按下→抬起允许的位移 |

队列在 64 深度下占约 2.3 KB 静态内存，MCU 上可以调到 8~16。

---

## 11. 常见问题

**Q：点了按钮没反应**
1. 主循环里有没有 `ql_input_dispatch()`？
2. 后端 `XSelectInput` / `wl_seat` 有没有拿到 pointer？
3. 按钮上面是不是压了一个标签且没设 `pointer-events: 0`？
   （标签默认吃命中，用 `ql_widget_set_pointer_events(label, 0)` 穿透）
4. 控件是不是 `visible = 0` 或父控件 `clip_children` 把它裁掉了？

**Q：`CLICK` 不触发但 `POINTER_DOWN` 触发了**
按下到抬起之间位移超过 4px，或抬起时目标变了（比如手指拖出去了）。

**Q：`POINTER_UP` 收不到**
按住时事件会发给按下目标（隐式捕获），所以目标不会变；如果完全收不到，
检查 `POINTER_LEAVE` 是否把 `pressed` 清了（这是有意为之）。

**Q：键盘事件没人收到**
没有焦点。先 `ql_input_set_focus(obj)`；`KEY_*` 目标是焦点控件，没有焦点则发给输入根。

**Q：Wayland 上按字母没反应**
M1 用 evdev → US 布局静态表，非 US 布局或其他输入法要等 M2 接 xkbcommon。
