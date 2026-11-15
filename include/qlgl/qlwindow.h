/**
 * @file    qlwindow.h
 * @brief   可选的"窗口 + 主循环"外壳：把平台初始化、输入、IME、剪贴板、
 *          脏区重绘都收进一个对象里。
 *
 * 目标是把最小程序压到十行左右：
 *
 * ```c
 * qlgl_init();
 * QLUIDoc *doc = ql_ui_load_html_file("index.html", "style.css");
 * QLUIRoot *ui = ql_ui_build(doc);
 * QLWindow *win = ql_window_create("Hello", 360, 200, QL_WINDOW_GL);
 * ql_window_bind_ui(win, ui);
 * ql_window_run(win);                 // 阻塞到关窗
 * ql_window_destroy(win);
 * ql_ui_free(ui);
 * ql_ui_doc_free(doc);
 * qlgl_shutdown();
 * ```
 *
 * 不想用默认主循环时也可以自己驱动：
 *
 * ```c
 * ql_window_show(win);                // 只显示，不阻塞
 * while (ql_window_poll(win)) {
 *     // ...自己的逻辑...
 * }
 * ```
 *
 * 注意：这个对象**不拥有** `QLUIRoot`。`ql_ui_free()` 仍由调用方负责
 * （doc 的所有权也分三种：静态 / blob / 运行期文本）。
 *
 * MCU 上没有窗口，只用 `ql_disp_*` + `ql_ui_render_canvas()`，不会链接到这里。
 */
#ifndef QLGL_WINDOW_H
#define QLGL_WINDOW_H

#include "qlgl.h"

typedef struct QLWindow QLWindow;

typedef enum {
    QL_WINDOW_GL = 0,      /* GPU：EGL + GLES3（X11 / Wayland） */
    QL_WINDOW_CPU = 1,     /* CPU 光栅（X11 / Wayland）；目前未实现 */
    QL_WINDOW_OFFSCREEN = 2, /* 不开窗口：离屏 FBO，配合 dump 用 */
} QLWindowBackend;

/* 事件过滤器：返回 1 = 已消费，不再交给界面层（与 ql_input_set_sink 同语义） */
typedef int (*ql_window_event_fn)(QLUIRoot *ui, const QLInputEvent *ev, void *user);
/* 每帧钩子：返回 1 表示这一帧必须整屏重画 */
typedef int (*ql_window_frame_fn)(QLUIRoot *ui, QLWindow *win, void *user);

/*
 * 建窗口。失败返回 NULL（没有 EGL/GLES3/X11/Wayland，或 CPU 后端未实现）。
 * `backend` 为 `QL_WINDOW_GL` 时窗口立刻可见（首帧由 `run/poll/render` 提交）。
 */
QLWindow *ql_window_create(const char *title, ql_coord_t width, ql_coord_t height,
                           QLWindowBackend backend);

/*
 * 绑界面。会做三件事：`ql_ui_attach_input()`、接剪贴板、接输入法，
 * 并按窗口尺寸布局一次。**必须在 create 之后、run 之前**调用。
 */
ql_err_t ql_window_bind_ui(QLWindow *win, QLUIRoot *ui);

/* 只显示（非阻塞）。默认主循环里 `run()` 会自己处理，不需要单独调。 */
void ql_window_show(QLWindow *win);

/* 跑默认主循环：阻塞到关窗 / `ql_window_quit()`。返回退出码（目前恒 0）。 */
int ql_window_run(QLWindow *win);

/*
 * 带命令行便利参数的主循环（例程/CI 用；应用层直接用 `ql_window_run()`）：
 *   --dump FILE     渲染一帧后退出（环境变量 QL_EX_DUMP 同义）
 *   --frames N      渲染 N 帧后退出（隐含 --spin）
 *   --seconds S     跑 S 秒后退出
 *   --stats         退出时打帧时间 p50/p95/max 与等 swap 时间
 *   --spin          每帧都渲染（性能测量用）
 *   --vsync 0|1     开关垂直同步
 */
int ql_window_run_args(QLWindow *win, int argc, char **argv);

/*
 * 换一份界面（live reload / 切页面）：摘掉旧 root 的输入汇，接上新 root，
 * 并按当前窗口尺寸重新布局。旧 root 仍归调用方所有。
 */
ql_err_t ql_window_set_ui(QLWindow *win, QLUIRoot *ui);

/* 手动循环：处理一轮事件 + 有脏区才画；返回 0 = 窗口关了。 */
int ql_window_poll(QLWindow *win);

/* 强制画一帧（整屏）。手动循环或截图时用。 */
void ql_window_render(QLWindow *win);

void ql_window_quit(QLWindow *win);
void ql_window_destroy(QLWindow *win);

/* 可选配置（在 bind/run 之前调用） */
void ql_window_set_vsync(QLWindow *win, int on);
void ql_window_set_bg(QLWindow *win, ql_color_t color);
void ql_window_set_event_filter(QLWindow *win, ql_window_event_fn fn, void *user);
void ql_window_set_frame_cb(QLWindow *win, ql_window_frame_fn fn, void *user);

#if defined(QLGL_ENABLE_GL)
/* 需要低层能力（zoom / 自定义绘制）时取后端句柄；CPU 后端返回 NULL */
QLGfxGL *ql_window_gfx(QLWindow *win);
#endif

/* 当前窗口的 CSS px 尺寸 / 绑定的根 */
void      ql_window_size(const QLWindow *win, ql_coord_t *out_w, ql_coord_t *out_h);
QLUIRoot *ql_window_ui(const QLWindow *win);

/* 把当前帧读回成 PPM（P6）。成功返回 1。离屏后端不依赖窗口系统。 */
int ql_window_dump_ppm(QLWindow *win, const char *path);

/*
 * 读整个文件。`ql_file_read_text()` 返回 NUL 结尾文本，`ql_file_read()` 返回原始
 * 字节（图片/字体用）。都用 `ql_mem_alloc` 分配，调用方 `ql_mem_free`。
 * 裸机没有文件系统：返回 NULL。
 */
char *ql_file_read_text(const char *path);
void *ql_file_read(const char *path, size_t *out_len);

#endif /* QLGL_WINDOW_H */
