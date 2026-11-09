# QLInput / 交互系统 阅读指南

> 目标读者：要读懂或修改 `src/core/qlinput.c`、给新后端接输入、或调试"点了没反应"的人。
> 只想用 API，请看 [QLInput API手册](./QLInput%20API手册.md)。

---

## 1. 文件地图

| 文件 | 职责 | 顺序 |
|---|---|---|
| `include/qlgl/qlinput.h` | `QLInputEvent`、`QLKey`、队列与事件汇接口 | ① |
| `src/core/qlinput.c` | 环形队列 + 坐标/按钮/修饰键跟踪 + 派发（198 行） | ② |
| `src/port/qlinput_x11.h` | X11 事件 → `QLInputEvent`（X11 与 GL 后端共用） | ③ |
| `src/port/qlinput_evdev.h` | evdev 扫描码 → `QLKey`（Wayland 用，US 布局） | ④ |
| `src/port/qldisp_x11.c` | `XSelectInput` + 事件轮询里调用 `ql_x11_push_input` | ⑤ |
| `src/port/qldisp_wayland.c` | `wl_pointer` / `wl_keyboard` 监听器 | ⑥ |
| `src/port/qlgfx_gl.c` | GL 窗口的 X11 事件同样走 `ql_x11_push_input` | ⑦ |
| `src/ui/qlui.c` | `ql_ui_input_event`：命中测试 + `:hover/:active/:focus` + 点击合成 | ⑧ |
| `test/input/test_input_queue.c` | 队列语义（顺序、溢出、状态跟踪、汇） | ⑨ |

> 命中测试**不在这里**。它在 `src/ui/qlui.c`，因为只有界面层认识
> HTML/CSS 编译出来的节点树（`display:none`、`overflow` 裁剪、`transform`、热区）。

---

## 2. 为什么队列要独立一层

后端只认识"坐标 + 按钮 + 键码"，界面层只认识节点树。中间隔一个**定长队列**，
换来三件事：

```
X11/Wayland 事件 ──┐
测试注入的事件   ──┼─→ QLInputEvent 队列 ─→ ql_input_dispatch() ─→ 事件汇 ─→ ql_ui_input_event()
自动化脚本       ──┘
```

1. **后端与界面解耦**：后端不 include 任何界面头文件，加一个新后端只写翻译代码；
2. **时序可预测**：所有输入都在主循环的固定位置派发（而不是在 `poll()` 里），
   所以"一帧内的输入按顺序处理完再重绘"成立；
3. **测试能注入**：`test_input_queue` 与界面层的输入测试都靠 `ql_input_push*`
   走真实路径，而不是直接调内部函数。

---

## 3. 事件队列

```c
QLInputEvent s_queue[QL_CFG_INPUT_QUEUE];   /* 默认 64，静态分配 */
uint16_t s_head, s_tail, s_count;           /* 环形队列 */
```

- 后端在 `poll()` 里 **只 push**，不派发；主循环每帧调一次 `ql_input_dispatch()`；
- 队列满时**丢最旧的**并返回 `QL_ERR_FULL`：交互事件宁可丢历史，也不能卡住输入；
- 回调里继续 push 是安全的：`ql_input_dispatch` 是"取一条、派发一条"的循环。

**为什么不在 poll 里直接派发？**
派发会调用户回调，用户回调里可能改控件、甚至销毁控件。放在主循环的固定位置，
时序可预测，也让"一帧内的所有输入"能按顺序处理完再重绘。

---

## 4. 界面层接手之后的四件事

队列把事件交出去之后，`ql_ui_input_event()` 做这些（细节见
[QLUI 阅读指南](../ui/QLUI阅读指南.md)）：

| 步骤 | 说明 |
|---|---|
| 命中测试 | 自顶向下、后挂的在上；`display:none` 整棵跳过；`overflow` 裁掉的部分不参与 |
| 状态位 | `:hover` 沿祖先链生效；`:active` 在按下到抬起之间保持；`:focus` 由点击/程序设置 |
| 点击合成 | 按下与抬起命中同一节点才算点击；滚轮/拖动不算 |
| 派发热区 | 命中 `<img>` 的 `<area>` 时派发 `area_cb` 而**不是** `click_cb` |

---

## 5. 事件参数约定

`QL_EMIT_*` 的参数是定长数组，所以每个事件的参数个数与顺序必须写死。
下表就是契约：

| 事件 | 参数 |
|---|---|
| `POINTER_ENTER` / `POINTER_LEAVE` | `(int x, int y)` |
| `POINTER_MOVE` | `(int x, int y, int buttons)` |
| `POINTER_DOWN` / `POINTER_UP` / `CLICK` | `(int x, int y, int button)` |
| `WHEEL` | `(int x, int y, int dy)` |
| `KEY_DOWN` / `KEY_UP` | `(uint key, int mods)` |
| `TEXT` | `(const char *utf8)` |
| `FOCUS_IN` / `FOCUS_OUT` | 无 |

坐标是**绝对逻辑坐标**（窗口左上角为原点），不是相对目标控件的坐标。

> 事件默认带 `BUBBLE | STOPPABLE | DEFAULT` 标志（没在类的事件描述表里登记），
> 所以 `ql_event_stop_propagation()` 有效，事件会冒泡到根。

---

## 6. 后端怎么接

后端只需要两件事：**选对事件掩码**、**把事件翻译成 `QLInputEvent`**。

### X11（`QSelectInput` 必须包含）

```c
ExposureMask | StructureNotifyMask |
KeyPressMask | KeyReleaseMask |
ButtonPressMask | ButtonReleaseMask |
PointerMotionMask | EnterWindowMask | LeaveWindowMask
```

翻译逻辑集中在 `src/port/qlinput_x11.h`（`ql_x11_push_input`），
X11 显示后端和 GL 后端共用，键码表只有一份。

要点：

- 滚轮是 `Button4`/`Button5`：**不是**按钮按下，要转成 `WHEEL` 且 `dy` 上下取反；
- `MotionNotify` 里用 `state & Button1Mask` 反推当前按住的按钮，写回 `buttons`；
- `EnterNotify` 当一次 MOVE 处理（否则从窗口外移入不会触发 hover）。

### Wayland

`wl_seat` 能力查询里分别取 `wl_keyboard` 与 `wl_pointer`（两者可以只有其一）：

- `pointer.enter/motion` → `ql_input_push_move`（坐标是 surface-local，直接可用）
- `pointer.button` → evdev 的 `BTN_LEFT/BTN_MIDDLE/BTN_RIGHT` 映射成 `QL_BUTTON_*`
- `pointer.axis` → `wl_fixed` 滚动量 × 20 折成"行"
- `pointer.leave` → `ql_input_push_leave`
- `keyboard.key` → evdev 码经 `qlinput_evdev.h` 转 `QLKey`；
  可打印字符**顺带合成 `TEXT`**（没有 xkbcommon 时按 US 布局近似）
- `keyboard.modifiers` → `dep` 位掩码 1/4/8/64 映射成 SHIFT/CTRL/ALT/META

> 坑：`wl_seat` 按 **version 1** 绑定，所以 pointer/keyboard/seat 都是 v1 对象。
> 清理时必须用 `wl_pointer_destroy` / `wl_keyboard_destroy` / `wl_seat_destroy`；
> `*_release` 是 v3+ 才有的请求，在 v1 上发出去就是"不存在的方法"。

### 关闭窗口

**后端不把 Esc/q 当关闭键**（否则应用自己收不到 Esc）。
需要"按 Esc 退出"就在自己装的汇里判断（`example/common.h` 就是这么做的）：

```c
static void sink(const QLInputEvent *ev, void *user)
{
    if (ev->type == QL_INPUT_KEY_DOWN && ev->key == QL_KEY_ESCAPE) {
        ql_gfx_gl_close(gfx);
        return;
    }
    ql_ui_input_event((QLUIRoot *)user, ev);
}
```

---

## 7. 顺带修掉的一个真 bug：子链表顺序

原来 `ql_object_set_parent` 是**头插**（`child->next_sibling = parent->first_child`），
而绘制从 `first_child` 正序遍历 —— 于是**最后挂上的节点画在最下面**，
和"后挂的子节点覆盖先挂的"的承诺相反。

命中测试一写就把这个矛盾暴露了（重叠区域总是命中最老的节点）。
处理方式：改成**追加到链尾**，让"链表顺序 == 绘制顺序"，
每对象省一个 `last_child` 指针（挂接只在构建期发生，O(n) 尾查找可接受）。

---

## 8. 已知边界

| 项 | 现状 | 计划 |
|---|---|---|
| Tab 键遍历焦点 | 未实现，焦点由程序设 | M1.5 |
| 双击 / 长按 / 拖拽手势 | 未合成（`time_ms` 字段已留） | M2 |
| 触摸 / 多点 | 未定义 | M5 |
| 祖先链的 ENTER/LEAVE | 只在目标上发，状态位已覆盖祖先 | 按需补 |
| Wayland 键位 | US 布局静态表 | M2 接 xkbcommon |
| IME / 组合输入 | 无 | M2 |
| 逻辑坐标 vs DPR | 目前 1:1，后端坐标即逻辑坐标 | M5 |

---

## 9. 建议阅读顺序

1. `include/qlgl/qlinput.h`（先看 `QLInputEvent` 和 `QLKey`）
2. `src/core/qlinput.c`（198 行：队列 + 状态跟踪 + 派发，一次读完）
3. `src/port/qlinput_x11.h`（一个后端怎么翻译事件）
4. `test/input/test_input_queue.c`（所有队列语义的可执行说明）
5. 想看"命中之后发生什么"：`src/ui/qlui.c` 的 `ql_ui_pointer_down/up/move`
