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

- 平台窗口（X11 / Wayland + EGL/GLES3）与高 DPI 处理；
- `setlocale()`（XIM 需要）与桌面系统字体自动发现（`qlgl_init()`）；
- `ql_ui_attach_input()`、剪贴板、输入法预编辑与候选窗定位；
- 保留式画布 + 脏区重绘：没有脏区就阻塞等事件（静止时 0 帧、0% CPU）；
- 窗口缩放时重新布局；`--vsync`、`--dump` 等开发参数。

应用层不需要知道后端是 X11 还是 Wayland，也不需要写帧循环。

## 2. API

```c
typedef enum {
    QL_WINDOW_GL = 0,        /* GPU（EGL + GLES3） */
    QL_WINDOW_CPU = 1,       /* CPU 光栅（尚未实现） */
    QL_WINDOW_OFFSCREEN = 2, /* 离屏 FBO，截图/CI */
} QLWindowBackend;

QLWindow *ql_window_create(const char *title, ql_coord_t w, ql_coord_t h,
                           QLWindowBackend backend);
ql_err_t  ql_window_bind_ui(QLWindow *win, QLUIRoot *ui);
ql_err_t  ql_window_set_ui(QLWindow *win, QLUIRoot *ui);  /* live reload */
void      ql_window_show(QLWindow *win);
int       ql_window_run(QLWindow *win);         /* 阻塞到关窗 */
int       ql_window_run_args(QLWindow *win, int argc, char **argv);
int       ql_window_poll(QLWindow *win);        /* 手动循环：1 = 继续 */
void      ql_window_render(QLWindow *win);      /* 强制画一帧 */
void      ql_window_quit(QLWindow *win);
void      ql_window_destroy(QLWindow *win);

void ql_window_set_vsync(QLWindow *win, int on);
void ql_window_set_bg(QLWindow *win, ql_color_t color);
void ql_window_set_event_filter(QLWindow *win, ql_window_event_fn fn, void *user);
void ql_window_set_frame_cb(QLWindow *win, ql_window_frame_fn fn, void *user);

void      ql_window_size(const QLWindow *win, ql_coord_t *w, ql_coord_t *h);
QLUIRoot *ql_window_ui(const QLWindow *win);
QLGfxGL  *ql_window_gfx(QLWindow *win);          /* 低层能力（zoom 等） */
int       ql_window_dump_ppm(QLWindow *win, const char *path);
```

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
| `--vsync 0/1` | 开关垂直同步 |

应用层直接用 `ql_window_run()` 即可，不需要这些参数。

## 5. 手动循环与钩子

```c
ql_window_show(win);
while (ql_window_poll(win)) {
    /* 每帧逻辑放这里（poll 已处理事件/动画/脏区并画完） */
}
```

- `ql_window_set_event_filter()`：事件先经过你，返回 1 表示消费（例程 38 用它滚背景）；
- `ql_window_set_frame_cb()`：每帧回调，返回 1 强制整屏重画（例程 20 热重载、37 缩放）；
- 需要更底层（zoom、自定义 GL 绘制）时 `ql_window_gfx()` 取 `QLGfxGL *`。

## 6. 限制

- `QL_WINDOW_CPU` 尚未实现；没有 GL 的桌面环境请用 `ql_disp_*` + `ql_ui_render_canvas()`；
- 输入汇是**进程级单例**：一进程一窗口（与 MCU 一致）；
- `QLWindow` 不拥有 `QLUIRoot`，也不管理 doc 的生命周期。
