# 命名与 API 风格

> 状态：**约定（2026-11-15）**。适用于所有公开头文件（`include/qlgl/*.h`）与
> 库内实现；`tools/check_style.py` 会在 `ctest` 里检查 typedef 部分。
> 相关：[QLWindow API 手册](./手册/ui/QLWindow%20API手册.md)、
> [平台后端设计](./平台后端设计.md)。

这份约定只解决一件事：**看到名字就知道它是哪一类东西**。
不追求"哪种更好看"，追求"12 个月后还能一眼分类"。

---

## 0. 一张表

| 类别 | 规则 | 例子 |
|---|---|---|
| 结构体 / 句柄 / 枚举 | **`QL` + CamelCase** | `QLRect`、`QLWindow`、`QLInputEvent`、`QLColorFormat` |
| 标量别名 | **`ql_` + snake_case + `_t`** | `ql_coord_t`、`ql_color_t`、`ql_fixed_t` |
| 回调 typedef | **`ql_<模块>_<动作>_cb`** | `ql_window_event_cb`、`ql_ui_click_cb`、`ql_event_cb` |
| 函数 | **`ql_<模块>_<动作>()`** | `ql_window_poll()`、`ql_canvas_fill_rect()` |
| 宏 / 常量 | **`QL_` + 大写** | `QL_COLOR_RGB`、`QL_CFG_INPUT_QUEUE` |
| 枚举成员 | **`QL_<类型>_<名字>`** | `QL_COLOR_RGB565`、`QL_WINDOW_GL` |
| 字段 / 局部变量 | **snake_case** | `border_box`、`scale_fx` |
| 布尔 | **`int`，0/1** | 公共头不引入 `stdbool.h` |

一句话：**类型是 `QL...` 或 `ql_..._t`，回调用 `_cb`，其余一律小写下划线。**

---

## 1. 为什么标量保留 `_t`

| 考虑 | 结论 |
|---|---|
| POSIX 保留 `_t` 结尾的标识符 | 确实保留，但那是"标准将来可能用"的保留；`ql_` 前缀已经把冲突面缩到零。`size_t` / `int32_t` 本身就是这个约定 |
| 枚举为什么不带 `_t` | 代码里 34 个枚举全部是 `QL...`（`QLColorFormat`、`QLInputType`…），只有 `ql_err_t` 一个是 `_t`。本次把它改成 `QLErr`，让"枚举 = CamelCase"没有例外 |
| 和结构体区分 | `ql_coord_t x` 与 `QLRect r` 在函数签名里一眼能分出"标量 / 对象" |
| 全量改名成本 | `ql_coord_t` 2652 处、`ql_color_t` 359 处……合计 **3312 处 / 110 个文件**，收益只是形式统一 |
| 小写无后缀方案 | `ql_event` 会与已有 `QLEvent` 结构体、`ql_event_*` 函数打架，实际得写成 `ql_event_id`；等于换个后缀继续存在 |

所以：**保留 `_t`，不再讨论。**

### 唯一例外：`QLType`

`QLType` 是"运行时类型 id"（`ql_object_get_type()` 的返回值），CamelCase 是
有意为之 —— 和 GTK 的 `GType`、Qt 的 `QMetaType::Type` 同一类东西，
它更像一个"类型对象"而不是普通标量。**保留，不改。**

---

## 2. 为什么回调统一成 `_cb`

历史上同时存在五种后缀：

| 后缀 | 旧例子 | 现在 |
|---|---|---|
| `_fn` | `ql_window_event_fn`、`ql_anim_time_fn`、`ql_draw2d_glyph_fn` | `_cb` |
| `_cb` | `ql_ui_click_cb` | 保持 |
| `_cb_t` | `ql_event_cb_t` | `ql_event_cb` |
| `_sink` | `ql_input_sink` | `ql_input_event_cb` |
| `_hook` | `ql_object_tree_hook` | `ql_object_tree_cb` |

选择 `_cb` 而不是 `_fn` 的理由：

1. **语义准确**：这些类型全部是"某件事发生时被调用"，`callback` 比
   `function` 更具体；`ql_anim_time_cb`（时钟源）读起来也不别扭。
2. **一个后缀**：`grep 'typedef.*(\*' include/qlgl/*.h` 出来必须全是 `_cb`，
   不用再记"这个模块用 fn、那个模块用 cb"。
3. **不再出现双后缀**：`_cb_t` 这种"回调 + 标量"的混合写法消失。

> `ql_input_set_sink()` **函数名**保持不变：sink 是"事件汇"这个概念，
> 类型叫 `ql_input_event_cb` 只是说它是个回调。

---

## 3. 反例（本次统一掉的）

```c
/* ❌ 结构体用了小写 */
typedef struct { ... } ql_rect;

/* ❌ 标量用了 CamelCase（QLType 除外） */
typedef uint32_t QLColor;

/* ❌ 回调没有 _cb 后缀 */
typedef void (*ql_window_event_fn)(QLUIRoot *, const QLInputEvent *, void *);
typedef void (*ql_input_sink)(const QLInputEvent *, void *);

/* ❌ 双后缀 */
typedef void (*ql_event_cb_t)(QLEvent *event);
```

```c
/* ✅ */
typedef struct { ... } QLRect;
typedef uint32_t ql_color_t;
typedef void (*ql_window_event_cb)(QLUIRoot *, const QLInputEvent *, void *);
typedef void (*ql_input_event_cb)(const QLInputEvent *, void *);
typedef void (*ql_event_cb)(QLEvent *event);
```

---

## 4. 检查

```bash
python3 tools/check_style.py          # 扫 include/ 与 src/ 的头文件
ctest -R test_style --output-on-failure
```

检查内容（只针对 typedef，避免误报）：

1. 函数指针 typedef 必须以 `_cb` 结尾；
2. 其它 typedef 必须是 `QL...`（CamelCase）或 `ql_..._t`；
3. 白名单：`QLType`。

新加类型时先过一遍这个脚本；它比 code review 更不容易漏。

---

## 5. 变更流程

- **1.0 之前**：直接改名，不留兼容别名（现在没有外部用户，越早越便宜）。
- **1.0 之后**：新增名字 + 保留旧名字的 `typedef` 别名，标注 `@deprecated`，
  至少一个大版本后再删。
- 任何一次批量改名都必须：全量构建（Debug / ASan / Release）+ 92 个测试 +
  MCU 构建 + 文档 HTML 重新生成，一个都不能少。
