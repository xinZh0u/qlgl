# QLAnim / 帧时钟与补间动画 阅读指南

> 本模块只管"时间怎么推进、值怎么插值"，**不管被插值的值是什么**。
> 颜色、透明度、绘制偏移都只是 4 个 float。
> 目标读者：要加新缓动、新动画、或把动画接到新后端的人。只想用，请看 [QLAnim API手册](./QLAnim%20API手册.md)。

---

## 1. 文件地图

| 文件 | 职责 |
|---|---|
| `include/qlgl/qlanim.h` | `QLAnimKey` / `QLAnimValue` / `QLEasing` / 全部函数声明 |
| `src/core/qlanim.c` | 帧时钟 + 缓动求值 + 动画池 + tick |
| `src/ui/qlui.c` 的"过渡段" | 消费本模块：把 `transition` 变成 `QLAnimKey`，插值结果写进 `QLUITrans.style` |
| `test/draw/test_anim.c` | 64 项断言：推进、重基、缓动端点/单调、steps、取消、池满、时钟回绕 |
| `test/web/test_transition.c` | 41 项断言：CSS `transition` 端到端 |

依赖方向：`qlanim` 不依赖 `qlui`、不依赖任何绘制后端，只依赖 `qlgl.h` 的 `ql_err_t`。
**反向依赖是单向的**：`qlui` → `qlanim`。

---

## 2. 三条设计约束

规划 §M5.5 定了三条，逐条对应实现：

### 2.1 没有动画时整帧跳过

`ql_anim_tick()` 返回"仍在进行中的动画数"。返回值是 0 时，主循环可以不重绘、不提交：

```c
if (ql_anim_tick(ql_anim_now_ms()) > 0)
    ql_ui_draw(ui, dw);
```

这一条对 MCU 是硬要求（没动画就不能烧 CPU），对 PC 是省电（笔记本风扇）。
**注意返回语义是"仍在进行"**，不是"这一帧有变化"：最后一个动画结束的那一 tick 会返回 0，
但它已经 apply 过终值并置了脏区，所以主循环**还要再画一帧**才进入静止。这条靠
"脏区标记"而不是靠返回值保证正确性，返回值只用来省"没有必要的那一帧"。

### 2.2 重基从当前值开始

`QLAnimKey.cur` 既是当前值，也是新动画的起点。`ql_anim_start()` 不从旧目标、也不从旧起点开始，
而是从 `key->cur` 开始：

```
A ──────▶ (到 40%) ── 目标改成 C
             │
             └─ cur 在这里，新动画从这个值接着走 → 无跳变
```

这是 CSS transition 被"打断"时的正确行为（浏览器也是这么做的）。
**同一个 key 重复 start 不需要先 cancel**，`start` 自己会找到已有槽位并重基。

### 2.3 只改绘制期属性

动画不进布局。想动画化 `width` 会触发每帧重排，性能模型直接崩。所以本模块的时间单位是"帧"，
但**进度不参与布局输入**——调用方（`qlui` 的 `trans_apply`）只写颜色字段与 alpha 字段。

---

## 3. 帧时钟

```c
uint32_t ql_anim_now_ms(void);
void     ql_anim_set_time_source(ql_anim_time_fn fn);
```

- 默认 `clock_gettime(CLOCK_MONOTONIC)`，精度毫秒，`uint32_t` 装（约 49.7 天回绕）。
- 测试用假时钟：`ql_anim_set_time_source(fake_now)`，把"当前时间"变成全局变量。
  **踩过的坑**：交互前必须把假时钟设成当前时刻，否则 `ql_anim_start` 记下的 t0 与
  测试随后 tick 的时间不同步，过渡会立刻被判定为"已结束"。
- 回绕用无符号减法：`now - t0` 在 `uint32_t` 上天然正确，**禁止**写 `if (now < t0) ...`。

---

## 4. 缓动

| 名字 | 对应 CSS | 曲线 |
|---|---|---|
| `QL_EASE_LINEAR` | `linear` | t → t |
| `QL_EASE_IN` | `ease-in` | cubic-bezier(0.42, 0, 1, 1) |
| `QL_EASE_OUT` | `ease-out` | cubic-bezier(0, 0, 0.58, 1) |
| `QL_EASE_IN_OUT` | `ease-in-out` | cubic-bezier(0.42, 0, 0.58, 1) |
| `QL_EASE_CSS` | `ease` | cubic-bezier(0.25, 0.1, 0.25, 1) |
| `QL_EASE_STEPS` | `steps(n)` | 阶跃，`n` 从 `QLAnimKey.steps` 来 |

`bezier_solve()` 的解算：先牛顿迭代 8 次，导数接近 0 时退出，**迭代次数固定**
（不按误差循环），这样同一输入永远得到同一输出——可测试、可复现、时长可预测。
两个端点做硬截断（`t<=0 → 0`、`t>=1 → 1`），避免浮点误差让终值差一点点。

`steps()` 用的是 `steps(n, end)` 语义：在 `k/n` 处**跳变到** `(k+1)/n`，
即 `t=0` 时为 0、`t=1` 时为 1。

---

## 5. 动画池

```c
static QLAnimSlot s_slots[QL_CFG_ANIM_MAX];   /* 默认 64 */
```

- **静态数组，不 malloc**。这是 MCU 与"启动后零分配"的要求。
- 槽位满（`QL_ERR_NOMEM`）时**调用方应当直接落到终值**，不排队。
  排队会让动画延迟累积，视觉上比"跳一下"更糟。`qlui` 里就是这个策略。
- 槽位归 `QLAnimKey*` 所有：`ql_anim_cancel()` 释放；`qlgl_shutdown()` 走 `ql_anim_reset()`。
- `QLAnimKey` 由调用方持有并零初始化，`used` 字段是内部状态，调用方不要碰。

---

## 6. 与 `qlui` 过渡的接法

`qlui` 里一次 `transition` 会占用：

```
QLUITrans { QLUINodeRT *node; QLUIStyle style; QLAnimValue from, to; ... }
             └─ 目标节点      └─ 过渡中的样式（绘制时用它）  └─ 本模块只认这 4 个 float
```

- 过渡样式存在**独立池** `QLUIRoot.trans[]`，不占用 `QLUIStyle` 的 `style_copy`，
  避免与 flex 强制尺寸互相踩。
- 起点值从**当前绘制样式**取（不是从 base 样式取），这样打断链不会跳变——正好对上 §2.2。
- 每帧 `ql_ui_anim_tick(ui, now)` 推进，返回变化节点的脏区并集，供 `ql_ui_consume_dirty()` 取用。

---

## 7. 常见坑

1. **别把 `ql_anim_tick` 的返回值当成"要不要重绘"的唯一依据**（见 §2.1）。
2. `-Wconversion` 下 `float` ↔ `uint8_t` 必须显式转换。颜色插值用 `float` 存 0..255，
   写回 `QLUIStyle` 时 `(uint8_t)(v + 0.5f)` —— 必须四舍五入，截断会让渐隐永远淡不到 0。
3. `QLAnimKey` 是**调用方持有的栈/静态变量**，池里只存指针。对象析构前必须 `ql_anim_cancel`
   （`qlui` 在 `ql_ui_free` 里做了）。
4. `steps` 为 0 时按 1 处理，不要除零。
5. 缓动的可复现性依赖"固定迭代次数"，别为了精度改成 while 循环。

---

## 8. 建议的阅读 / 验证顺序

1. `include/qlgl/qlanim.h` 头注释（设计三条约束的原文）
2. `src/core/qlanim.c` 的帧时钟 → 缓动 → 池与 tick
3. `test/draw/test_anim.c`：看"重基不跳变"和"时钟回绕"两个用例怎么写的
4. `src/ui/qlui.c` 过渡段：看本模块被怎么消费
5. `test/web/test_transition.c`：看 CSS 到动画的端到端

```bash
ctest --test-dir build-rel -R test_anim
ctest --test-dir build-rel -R test_transition
```
