/**
 * @file    qldisp_x11.h
 * @brief   Linux X11 窗口显示后端。
 *
 * 渲染器直接写进 XImage 的像素存储，flush 时用 XPutImage 推脏矩形到窗口。
 * 使用方式：
 *
 *     QLDispDriver *d = ql_disp_x11_create("QLGL Demo", 320, 240,
 *                                          QL_COLOR_FORMAT_AUTO);
 *     if (d == NULL) { 无 X 环境，降级到内存驱动 }
 *     QLCanvas cv;
 *     ql_canvas_from_disp(&cv, d);
 *     while (ql_disp_x11_poll(d)) {
 *         ql_input_dispatch();              // 事件交给装了的事件汇（ql_ui）
 *         ql_ui_render_canvas(ui, &cv);     // 或 ql_ui_render(ui, d2d)
 *     }
 *     ql_disp_x11_destroy(d);
 *
 * 编译期由 CMake 定义 QLGL_ENABLE_X11；没有 X11 时本节内容不参与编译。
 */
#ifndef QLGL_DISP_X11_H
#define QLGL_DISP_X11_H

#include "qlgl.h"
#include "qldisp.h"

#if defined(QLGL_ENABLE_X11)

/*
 * 创建窗口。title 可为 NULL（默认 "QLGL"）。
 * 返回的驱动已经通过 ql_disp_set 设为活动显示。
 * 失败返回 NULL 并打错误日志（最常见原因是没有 DISPLAY）。
 */
QLDispDriver *ql_disp_x11_create(const char *title, ql_coord_t width, ql_coord_t height,
                                 QLColorFormat format);

/*
 * 处理挂起的窗口事件并返回是否继续：
 *   1  窗口仍在，可以继续渲染
 *   0  窗口被关闭（点了关闭按钮 / 按 Esc / DestroyNotify）
 * Expose 事件会自动把整屏标记为脏。
 */
int ql_disp_x11_poll(QLDispDriver *driver);

/*
 * 告诉输入法光标位置（候选窗定位），语义同 `ql_gfx_gl_set_ime_spot()`：
 * CSS px、视口坐标。每帧或每次光标移动后调一次即可（值没变不发 X 请求）。
 */
void ql_disp_x11_set_ime_spot(QLDispDriver *driver, ql_coord_t x, ql_coord_t y,
                              ql_coord_t h);

void ql_disp_x11_destroy(QLDispDriver *driver);

/* 请求关闭窗口：下一次 poll 返回 0，主循环随之退出 */
void ql_disp_x11_close(QLDispDriver *driver);

/* 窗口名（调试用），未创建返回 "" */
const char *ql_disp_x11_title(QLDispDriver *driver);

/* 已经处理过的 Expose 次数，测试/调试用 */
uint32_t ql_disp_x11_expose_count(QLDispDriver *driver);

/*
 * 窗口尺寸变过没有（取一次清零）。
 *
 * X11 后端在 ConfigureNotify 里**已经重建了帧缓冲**（`driver->width/height/stride/
 * framebuffer` 都是新的），但 QLCanvas 是这些字段的拷贝，所以返回 1 时调用方必须：
 *
 *     ql_canvas_from_disp(&canvas, driver);      // 重新取一份
 *     ql_ui_layout(ui, ql_disp_css_width(driver), ql_disp_css_height(driver));
 *
 * 漏掉这一步的表现是"窗口拉大之后内容还挤在左上角、右边一片空白"。
 */
int ql_disp_x11_resize_pending(QLDispDriver *driver);

#endif /* QLGL_ENABLE_X11 */

#endif /* QLGL_DISP_X11_H */
