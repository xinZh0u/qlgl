# QLAnim / 帧时钟与补间动画 API手册（v0.1）

配套阅读：[QLAnim 阅读指南](./QLAnim阅读指南.md)

> **适用场景**
> - 需要"值随时间平滑变化"的地方：颜色渐变、透明度、绘制偏移、自定义控件的状态动画。
> - **不适用**：动画化布局属性（`width`/`height`/`flex-*`）。那会每帧重排，本模块不支持，
>   也不会支持——要变形请改尺寸后再重排一次，别逐帧插值。
> - **不适用**：关键帧序列（`@keyframes`）。用同一个 key 链式 `start`，或在 `on_done` 里起下一段。

---

## 1. 最小可用例子

```c
#include <qlgl/qlgl.h>

static QLAnimKey s_alpha_key;

static void on_apply(void *user, const QLAnimValue *v)
{
    QLWidget *w = (QLLabel *)user;
    ql_label_set_alpha(w, (uint8_t)(v->v[0] + 0.5f));
}

void fade_in(QLLabel *w)
{
    QLAnimValue to = { .v = { 255.0f, 0.0f, 0.0f, 0.0f } };
    s_alpha_key.cur.v[0] = 0.0f;             /* 起点 = 当前值 */
    ql_anim_start(&s_alpha_key, &to, 200, QL_EASE_OUT, 0, on_apply, NULL, w);
}

/* 主循环 */
for (;;) {
    uint32_t now = ql_anim_now_ms();
    if (ql_anim_tick(now) > 0 || needs_redraw)
        render_once();
    else
        wait_for_event();                    /* 静止：不烧 CPU */
}
```

---

## 2. 类型

### 2.1 `QLAnimValue`

```c
typedef struct { float v[4]; } QLAnimValue;
```

4 个 float。颜色约定为 **0..255 的浮点**（本模块只做线性插值，不理解语义）。
只用一个标量时只填 `v[0]`。

### 2.2 `QLAnimKey`

```c
typedef struct {
    QLAnimValue cur;     /* 当前值 = 下一个动画的起点 */
    uint8_t     used;    /* 内部 */
    uint8_t     steps;   /* QL_EASE_STEPS 用 */
    uint16_t    reserved;
} QLAnimKey;
```

**由调用方持有（栈/静态/结构体成员），必须零初始化。** 池里只存指向它的指针，
所以 key 的生命周期必须覆盖整个动画；提前销毁前请 `ql_anim_cancel()`。

想在动画之外改值：直接写 `key->cur`，下次 `ql_anim_start` 会从那里接着动。

### 2.3 `QLEasing`

| 值 | CSS 对应 | 说明 |
|---|---|---|
| `QL_EASE_LINEAR` | `linear` | |
| `QL_EASE_IN` | `ease-in` | |
| `QL_EASE_OUT` | `ease-out` | 界面过渡的默认选择，响应感最好 |
| `QL_EASE_IN_OUT` | `ease-in-out` | |
| `QL_EASE_CSS` | `ease` | 浏览器默认值 |
| `QL_EASE_STEPS` | `steps(n)` | 台阶数来自 `QLAnimKey.steps`，`steps(n, end)` 语义 |
| `QL_EASE_COUNT` | — | 计数用 |

---

## 3. 函数

### 3.1 时钟

```c
uint32_t ql_anim_now_ms(void);
void     ql_anim_set_time_source(uint32_t (*fn)(void));
```

单调毫秒。默认 `CLOCK_MONOTONIC`；传 `NULL` 恢复默认。
MCU 上换成 tick 计数器即可（`HAL_GetTick()` 之类），只需保证**单调递增、回绕用无符号减法**。

测试里换假时钟：

```c
static uint32_t s_now;
static uint32_t fake_now(void) { return s_now; }

ql_anim_set_time_source(fake_now);
s_now = 1000;                       /* 交互前先同步，否则 t0 与 tick 不一致 */
ql_anim_start(&key, &to, 200, QL_EASE_LINEAR, 0, apply, NULL, NULL);
s_now = 1100;
ql_anim_tick(s_now);                /* 走到 50% */
ql_anim_set_time_source(NULL);
```

### 3.2 启动 / 重基

```c
ql_err_t ql_anim_start(QLAnimKey *key, const QLAnimValue *to, uint32_t dur_ms,
                       uint8_t ease, uint8_t steps,
                       ql_anim_apply_fn apply, ql_anim_done_fn done, void *user);
```

| 参数 | 说明 |
|---|---|
| `key` | 调用方持有、零初始化过 |
| `to` | 目标值 |
| `dur_ms` | 时长；**0 表示立即落到终值**（仍会走一次 apply + done） |
| `ease` | `QLEasing` |
| `steps` | 仅 `QL_EASE_STEPS` 用；0 按 1 处理 |
| `apply` | 每帧把插值结果写到目标；可为 `NULL` |
| `done` | 结束时调一次；可为 `NULL`（`dur_ms == 0` 时同帧调用） |
| `user` | 透传给两个回调 |

- 起始值取 `key->cur`。**同一个 key 重复调用 = 重基**，中途改目标不会跳变。
- 返回 `QL_OK` / `QL_ERR_INVAL`（参数非法）/ `QL_ERR_NOMEM`（池满）。
  **池满时请调用方自己落到终值**，不要排队：

```c
if (ql_anim_start(&k, &to, 180, QL_EASE_OUT, 0, apply, NULL, w) != QL_OK) {
    apply(w, &to);
    if (done) done(w);
}
```

### 3.3 推进

```c
uint32_t ql_anim_tick(uint32_t now_ms);
```

对每个活跃动画算插值 → `apply`；到时间就 apply 终值 → `done` → 释放槽位。
返回**仍在进行中**的动画数。

- 返回 0 = 这一帧之后没有运动了。**但"最后一个动画刚结束"的那一 tick 已经置了脏区**，
  所以主循环要么这一帧先画、下一帧再静止，要么用脏区标记兜底。
- 可以一帧调多次（比如补帧），也可以多帧不调（窗口最小化）——时间是从 `now_ms` 算的，
  不会"丢帧累积"。

### 3.4 取消与查询

```c
void     ql_anim_cancel(QLAnimKey *key);   /* 不再 apply，也不触发 done；key 可立刻复用 */
uint32_t ql_anim_active(void);             /* 当前活跃数 */
void     ql_anim_reset(void);              /* 清空全部（qlgl_shutdown 调用） */
```

`ql_anim_cancel(NULL)` 与对未启动的 key cancel 都是安全的空操作。

### 3.5 缓动求值

```c
float ql_ease_apply(uint8_t ease, uint8_t steps, float t);
```

`t ∈ [0,1] → [0,1]`，端点硬截断。测试与自定义插值复用，也方便画曲线。

---

## 4. 与 CSS `transition` 的关系

CSS 侧只需写标准语法，构建期由 `qlui_parse.c` 展开成 `QLUIStyle` 的
`trans_ms` / `trans_ease` / `trans_props`，运行期自动创建过渡槽：

```css
.btn        { transition: 180ms ease-out; }         /* 全部可过渡属性 */
.btn        { background-color: #204060; }
.btn:hover  { background-color: #2a9a5c; }
.card       { transition: background-color 300ms linear, opacity 120ms; }
```

目前可过渡的属性：`background-color`（`QL_UI_TRANS_BG`）、`opacity`（`QL_UI_TRANS_ALPHA`，
插值的是**背景 alpha**，不是 CSS 的组不透明度）、`all`（两者）。
`color` / `border-color` / `transform` / `delay` 生成器会**明确报错**，不静默降级。

运行期接口：

```c
uint32_t ql_ui_anim_tick(QLUIRoot *ui, uint32_t now_ms);  /* 推进 + 返回脏区并集 */
int      ql_ui_consume_dirty(QLUIRoot *ui, QLRect *out);  /* 取走脏区并清空 */
```

**注意**：`ql_ui_node_style()` 返回的是"过渡中的样式"。要取某状态的**目标值**
（比如给布局用），用内部 `style_for_state()`——布局相关字段不受过渡影响，
但从公开 API 读到的颜色可能是中间值。

---

## 5. 状态与限制

| 项 | 值 |
|---|---|
| 同时活跃动画上限 | `QL_CFG_ANIM_MAX`（默认 64），编译期可改 |
| 时间分辨率 | 1 ms，`uint32_t` 约 49.7 天回绕 |
| 每个动画的内存 | 槽位约 64 B（静态池，无分配） |
| 支持的值维度 | 4 个 float（颜色 RGBA 或 1 个标量） |
| 不支持 | 布局属性动画、关键帧序列、贝塞尔以外的自定义曲线、负时长（按 0 处理） |

---

## 6. 常见问题

**Q：为什么动画结束了画面还是旧值？**
`done` 在最后一帧被调用，如果 `apply` 只在 `tick` 里写目标，请确认 `done` 里也落了终值，
或让主循环在 `tick` 返回 0 后仍重绘一帧（靠脏区判断）。

**Q：`QL_ERR_NOMEM` 一直出现？**
key 用完后没 `cancel`，或者在一帧里给同一个节点起了多个互不取消的动画。
检查是否每次都 `start` 了新 key。

**Q：动画卡在中间不动？**
假时钟没同步（见 §3.1），或者 `ql_anim_tick` 被调用时传的时间没前进。

**Q：想要"先淡出再淡入"？**
在 `done` 回调里 `start` 下一段。同一个 key 重基从当前值开始，正好自然衔接。

**Q：能不能一帧插值两个值？**
一个 key 有 4 个 float，够放 RGBA。超过 4 个就开第二个 key。

---

## 7. 关键帧动画（`@keyframes`）

**不在本模块**。`@keyframes` 由 `qlui` 在构建期编译成静态帧表，运行期自己按时间求值，
只用本模块的 `ql_ease_apply()` 做缓动。

用 CSS 就够了，不需要写 C：

```css
.pulse { animation: pulse 1s linear infinite alternate; }
@keyframes pulse { from { background-color: #204060 } to { background-color: #ff8000 } }

/* 关键帧里也能插值平移 → 最省事的"滑入" */
@keyframes slidein {
  from { transform: translate(-70px, 0); opacity: 0; }
  to   { transform: translate(0, 0);     opacity: 1; }
}
.card { animation: slidein 400ms ease-out 1 forwards; }
```

想看效果：`./build/example_anim_window`（空格重播，Esc 退出）。

运行期重播某个节点的动画：

```c
ql_ui_anim_restart(ui, node);      /* 节点没有动画 → QL_ERR_INVAL */
```

详见 [QLUI API手册](../ui/QLUI%20API手册.md) §3.10 与
[QLAnim 阅读指南](./QLAnim阅读指南.md) §9。
