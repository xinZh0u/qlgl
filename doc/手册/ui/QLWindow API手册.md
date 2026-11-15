# QLWindow API 手册：窗口、输入与主循环

短版本（完整例程见 `example/00_hello`）：

```c
#include <qlgl/qlgl.h>

int main(void)
{
    if (qlgl_init() != QL_OK)
        return 1;

    QLUIRoot *ui = ql_ui_load_html_file("index.html", "style.css");
    QLWindow *win = ql_window_create("Hello", 640, 400, QL_WINDOW_GL);
    ql_window_bind_ui(win, ui);
    ql_window_run(win);

    ql_window_destroy(win);
    ql_ui_free(ui);
    qlgl_shutdown();
    return 0;
}
```

## 1. 它替你接了什么

`ql_window_create()` + `ql_window_bind_ui()` 内部完成：

- 平台窗口（桌面是 X11 / Wayland + EGL/GLES3，MCU 是板级帧缓冲，见 §6）与高 DPI 处理；
- `setlocale()`（XIM 需要）与桌面系统字体自动发现（`qlgl_init()`）；
- `ql_ui_attach_input()`、剪贴板、输入法预编辑与候选窗定位；
- 保留式画布 + 脏区重绘：没有脏区就阻塞等事件（静止时 0 帧、0% CPU）；
- 窗口缩放时重新布局；`--vsync`、`--dump` 等开发参数。

应用层不需要知道后端是 X11 还是 Wayland，也不需要写帧循环。

## 2. API

```c
typedef enum {
    QL_WINDOW_GL = 0,        /* GPU（EGL + GLES3） */
    QL_WINDOW_CPU = 1,       /* CPU 光栅（X11 / Wayland / 内存） */
    QL_WINDOW_OFFSCREEN = 2, /* 离屏，截图/CI */
} QLWindowBackend;

QLWindow *ql_window_create(const char *title, ql_coord_t w, ql_coord_t h,
                           QLWindowBackend backend);
QLErr  ql_window_bind_ui(QLWindow *win, QLUIRoot *ui);
QLErr  ql_window_set_ui(QLWindow *win, QLUIRoot *ui);  /* live reload */
int       ql_window_run(QLWindow *win);         /* 阻塞到关窗 */
int       ql_window_run_args(QLWindow *win, int argc, char **argv);
int       ql_window_poll(QLWindow *win);        /* 手动循环：1 = 继续 */
void      ql_window_render(QLWindow *win);      /* 强制画一帧 */
void      ql_window_quit(QLWindow *win);
void      ql_window_destroy(QLWindow *win);
QLErr  ql_window_resize(QLWindow *win, ql_coord_t w, ql_coord_t h);

void ql_window_set_vsync(QLWindow *win, int on);
void ql_window_set_bg(QLWindow *win, ql_color_t color);
void ql_window_set_event_filter(QLWindow *win, ql_window_event_cb fn, void *user);
void ql_window_set_frame_cb(QLWindow *win, ql_window_frame_cb fn, void *user);

void      ql_window_size(const QLWindow *win, ql_coord_t *w, ql_coord_t *h);
QLUIRoot *ql_window_ui(const QLWindow *win);
QLGfxGL  *ql_window_gfx(QLWindow *win);          /* 低层能力（zoom 等） */
int       ql_window_dump_ppm(QLWindow *win, const char *path);
```

## 2.1 多窗口

一个进程可以开多个窗口，每个窗口有自己的界面树、输入队列和剪贴板/输入法状态：

```c
QLWindow *a = ql_window_create("A", 320, 200, QL_WINDOW_GL);
QLWindow *b = ql_window_create("B", 320, 200, QL_WINDOW_GL);
ql_window_bind_ui(a, ui_a);
ql_window_bind_ui(b, ui_b);

while (a != NULL || b != NULL) {
    if (a != NULL && !ql_window_poll(a)) { ql_window_destroy(a); a = NULL; }
    if (b != NULL && !ql_window_poll(b)) { ql_window_destroy(b); b = NULL; }
}
```

要点：

- 每个 `QLWindow` 创建时拿到自己的 `QLInput`；平台后端（X11 / Wayland / GL）
  实现了 `QLPlatform.set_input` 才会按窗口路由事件。没实现的后端（假平台、
  MCU 板级）退回进程默认队列，等价于单窗口。
- 剪贴板与输入法也是每窗口一份状态：X11 用它自己的 `Display` 连接应答
  SelectionRequest，两个窗口互不覆盖。
- 关掉一个窗口只销毁它自己，另一个继续跑；事件过滤器里的 `ui` 参数就是
  该窗口自己的根。
- GL 后端每帧会切回自己的上下文（`eglMakeCurrent`），不需要调用方干预。
- 完整示例：`example/43_multi_window`（两个窗口、点击回调各自计数）。

## 3. 每次谁拥有谁

- `QLWindow` **不拥有** `QLUIRoot`：`ql_window_destroy()` 只拆窗口，`ql_ui_free()` 仍由调用方调；
- `ql_ui_load_html_file()` 返回的 root 拥有 doc，`ql_ui_free()` 一并释放；
- `ql_ui_load_file()`（`.qlui` blob）同样让 root 拥有 doc；
- 手动 `ql_ui_build(doc)` 时 root **不**持有 doc，需要 `ql_ui_doc_free(doc)`。

释放顺序建议：先 `ql_window_destroy()`（摘输入汇），再 `ql_ui_free()`。

## 4. 开发参数（`ql_window_run_args`）

| 参数 | 作用 |
|---|---|
| `--dump FILE` | 渲染一帧存 PPM 后退出（`QL_EX_DUMP` 环境变量同义） |
| `--frames N` | 渲染 N 帧后退出（隐含 `--spin`） |
| `--seconds S` | 跑 S 秒后退出 |
| `--stats` | 退出时打印帧时间 p50/p95/max 与等 swap 时间 |
| `--spin` | 每帧都渲染（性能测量用） |
| `--size WxH` | 改窗口尺寸（X11/GL 异步生效） |
| `--vsync 0/1` | 开关垂直同步 |

应用层直接用 `ql_window_run()` 即可，不需要这些参数。

## 5. 手动循环与钩子

```c
while (ql_window_poll(win)) {
    /* 每帧逻辑放这里（poll 已处理事件/动画/脏区并画完） */
}
```

- `ql_window_set_event_filter()`：事件先经过你，返回 1 表示消费（例程 38 用它滚背景）；
- `ql_window_set_frame_cb()`：每帧回调，返回 1 强制整屏重画（例程 20 热重载、37 缩放）；
- 需要更底层（zoom、自定义 GL 绘制）时 `ql_window_gfx()` 取 `QLGfxGL *`。

## 6. 平台后端

`QLWindow` 只依赖 `QLPlatform` 回调表（`<qlgl/qlplatform.h>`，设计见
[平台后端设计](../../平台后端设计.md)）：

```c
qlgl_set_platform(&my_platform);   // 在 qlgl_init() 之前；MCU / 测试 / 模拟器
qlgl_init();
```

- 不设置时用 `qlgl_platform_default()`（弱符号）：桌面构建是
  X11 → Wayland → 内存离屏，GL 失败可按 `QL_WINDOW_CPU` 走 CPU；
- MCU 板级文件定义同名强符号 `qlgl_platform_default()` 即可覆盖，
  应用层仍然只写 `qlgl_init()`；
- 参考实现：`test/mcu/qlplatform_board.c`（静态 RGB565 面板 + 按键环形队列 +
  SysTick 毫秒时钟），验收程序 `test/mcu/window.c`，`./test/mcu/build.sh window`
  一条命令在 QEMU 的 Cortex-M4 上跑完（后端覆盖、首帧、触摸、脏区、点击、时钟）；
- 调试：`QLGL_WINDOW_CPU=1 ./example_00_hello` 强制 CPU 光栅，与 GL 对照。

## 7. 限制

- **多窗口**：每个 `QLWindow` 有自己的输入队列（`QLInput`），后端实现了
  `QLPlatform.set_input` 时事件按窗口路由；没实现的后端（假平台、MCU 板级）
  退回进程默认队列，等价于单窗口。参考 `example/43_multi_window`；
- `QLWindow` 不拥有 `QLUIRoot`，也不管理 doc 的生命周期；
- Wayland（CPU 与 GL）不能由客户端直接改窗口尺寸，`ql_window_resize()` 返回
  `QL_ERR_UNSUPPORTED`；X11 是异步生效（ConfigureNotify）；
- 板级面板尺寸固定时 `ql_window_resize()` 同样返回 `QL_ERR_UNSUPPORTED`；
- 裸机没有 `clock_gettime`：窗口层自动退回平台的时间源（板级在 `window_open`
  里用 SysTick 设好），`--seconds`/帧统计因此是毫秒精度；
- `--stats` 的采样缓冲按需分配（3×1024 个 uint32 ≈ 12KB），MCU 上不开就不占堆。
