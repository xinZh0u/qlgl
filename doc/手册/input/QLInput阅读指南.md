# QLInput / 交互系统 阅读指南

> 目标读者：要读懂或修改 `src/core/qlinput.c`、给新后端接输入、或调试"点了没反应"的人。
> 只想用 API，请看 [QLInput API手册](./QLInput%20API手册.md)。

---

## 1. 文件地图

| 文件 | 职责 | 顺序 |
|---|---|---|
| `include/qlgl/qlinput.h` | `QLInputEvent`、`QLKey`、队列与派发接口 | ① |
| `src/core/qlinput.c` | 队列、命中测试、hover/pressed/focus 状态机、点击合成 | ② |
| `include/qlgl/qlwidget.h` | 控件事件 id（`QL_WIDGET_EVENT_*`）、状态位（`QL_WIDGET_STATE_*`） | ③ |
| `src/widget/qlwidget.c` | 状态位实现、状态背景色、`draw_default` 里的状态选色 | ④ |
| `src/port/qlinput_x11.h` | X11 事件 → `QLInputEvent`（X11 与 GL 后端共用） | ⑤ |
| `src/port/qlinput_evdev.h` | evdev 扫描码 → `QLKey`（Wayland 用，US 布局） | ⑥ |
| `src/port/qldisp_x11.c` | `XSelectInput` + 事件轮询里调用 `ql_x11_push_input` | ⑦ |
| `src/port/qldisp_wayland.c` | `wl_pointer` / `wl_keyboard` 监听器 | ⑧ |
| `src/port/qlgfx_gl.c` | GL 窗口的 X11 事件同样走 `ql_x11_push_input` | ⑨ |
| `src/ui/qlui.c` | `ql_ui_input_event`：把输入事件映射到 `QLUIRoot` 的 `:hover/:active` | ⑩ |
| `test/input/test_input.c` | 130 项断言，覆盖下面所有规则 | ⑪ |
| `test/input/example_input.c` | 可交互窗口示例 | ⑫ |

---

## 2. 为什么要有独立一层

后端只认识"坐标 + 按钮 + 键码"，**不认识控件树**。
输入层负责把原始事件变成有语义的控件事件，因此：

```
X11/Wayland 事件 ──┐
测试注入的事件   ──┼─→ QLInputEvent 队列 ─→ ql_input_dispatch() ─→ 命中测试 ─→ ql_emit()
自动化脚本       ──┘                                                          │
                                                            捕获 / 目标 / 冒泡（复用对象系统）
```

这样带来三个直接好处：

1. **无窗口也能测**：`test_input.c` 不开窗口，直接 push 事件，断言派发结果；
2. **后端极薄**：新增后端只写一个"翻译函数"，不碰交互逻辑；
3. **交互逻辑只有一份**：X11、Wayland、离屏、测试走同一条路。

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

## 4. 命中测试

`ql_input_hit_test(root, x, y)` 的关键规则，全部有测试覆盖：

1. **后挂的子控件在上层**
   子链表顺序 == 绘制顺序（`ql_object_set_parent` 追加到链尾，见 §8），
   所以命中测试递归时"最后一个命中的"就是视觉上最上层的。
2. **父控件先于子控件？** 不是。先递归子节点，子节点命中就返回子节点；
   子节点都没命中才考虑父节点自己。
3. **`pointer-events: none`**（`ql_widget_set_pointer_events(obj, 0)`）：
   该控件自己不吃命中，事件穿透到下层。标签放在按钮里时必须设成 0。
4. **不可见控件**（`visible = 0` 或 `QL_OBJ_FLAG_HIDDEN`）整棵子树跳过。
5. **`clip_children`**：父控件裁剪子控件时，超出父盒子的部分不参与命中
   （和绘制表现一致）。
6. 点在最外层的空白处 → 命中最外层根控件；点在根之外 → `NULL`。

递归实现（简化）：

```c
QLObject *hit = NULL;
for (child = first_child; child; child = next_sibling) {
    QLObject *found = hit_walk(child, x, y, box);   /* box = 本节点矩形 */
    if (found) hit = found;                        /* 后者覆盖前者 = 后者在上 */
}
if (hit) return hit;                               /* 子节点优先 */
return 本节点包含该点 ? 本节点 : NULL;
```

---

## 5. 状态机：hover / pressed / focus

三个全局状态（`s_hover` / `s_pressed` / `s_focus`）驱动控件上的三个状态位
（`QL_WIDGET_STATE_HOVER / ACTIVE / FOCUS`）。

**状态位沿父链设置**（`set_state_chain`），因为 CSS 的 `:hover` 对祖先同样成立：
鼠标停在按钮上时，`root`、`card`、`btn` 都带 `HOVER`。

### hover

```
MOVE(40,80) ──→ 命中 btn ──→ 与 s_hover 不同 ──→ 清旧链 HOVER / 设新链 HOVER
                                            └─→ 对旧目标发 POINTER_LEAVE
                                            └─→ 对新目标发 POINTER_ENTER
```

同一目标重复移动不重复发 ENTER（`s_hover == target` 直接返回），避免每帧刷事件。

### pressed（按下）

- `POINTER_DOWN` 记录 `s_pressed`、`s_press_x/y`，给目标链设 `ACTIVE`；
- **按住时移动**：事件仍然送给 `s_pressed`（等价 DOM 的隐式指针捕获），
  否则"按下按钮 → 拖出按钮 → 松手"会永远收不到 UP；
- `POINTER_UP` 清 `ACTIVE`、清 `s_pressed`；
- `POINTER_LEAVE`（指针离开窗口表面）也会清 `ACTIVE` + `s_pressed`，防止卡住。

### click 合成

`UP` 时若同时满足：

- 抬起目标 == 按下目标（`pressed == target`）
- 位移 ≤ `QL_CLICK_SLOP`（4 px）

才发 `CLICK`。所以"按下后拖到别处松手"不算点击。

### focus

`ql_input_set_focus(obj)` 负责发出 `FOCUS_OUT` / `FOCUS_IN` 并同步状态位。
键盘事件和文本输入的目标是焦点控件（没有焦点则回落到输入根），
因此**按键会沿控件树冒泡**，容器可以统一处理按键。

启动时**没有**默认焦点，也没有 Tab 遍历（M1 未做，见 §9）。

---

## 6. 事件参数约定

`QL_EMIT_*` 的参数是定长数组，所以每个事件的参数个数与顺序必须写死。
下表就是契约（`qlwidget.h` 里也有注释）：

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

## 7. 后端怎么接

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

**后端不再把 Esc/q 当关闭键**（否则应用自己收不到 Esc）。
需要"按 Esc 退出"就在应用里连一个 `KEY_DOWN` 处理器，再调
`ql_disp_x11_close()` / `ql_disp_wayland_close()` / `ql_gfx_gl_close()`。
`test/window_helper.h` 的 `ql_win_bind_esc()` 就是这么做的。

---

## 8. 顺带修掉的一个真 bug：子链表顺序

原来 `ql_object_set_parent` 是**头插**（`child->next_sibling = parent->first_child`），
而 `qlrender` 从 `first_child` 正序遍历绘制 —— 于是**最后挂上的控件画在最下面**，
和 `qlwidget.h` 里"后挂的子控件覆盖先挂的"的承诺相反。

命中测试一写就把这个矛盾暴露了（重叠区域总是命中最老的控件）。
处理方式：改成**追加到链尾**，让"链表顺序 == 绘制顺序"，
每对象省一个 `last_child` 指针（挂接只在构建期发生，O(n) 尾查找可接受）。

---

## 9. M1 的已知边界

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

## 10. 建议阅读顺序

1. `include/qlgl/qlinput.h`（先看 `QLInputEvent` 和 `QLKey`）
2. `src/core/qlinput.c` 的 `hit_walk`（命中测试的全部规则）
3. `handle_move` / `handle_button`（状态机 + 点击合成）
4. `ql_input_dispatch`（队列与 sink 的关系）
5. `src/widget/qlwidget.c` 的 `ql_widget_set_state` 与 `ql_widget_draw_default`
6. `src/port/qlinput_x11.h`（一个后端怎么翻译事件）
7. `test/input/test_input.c`（所有规则的可执行说明）
