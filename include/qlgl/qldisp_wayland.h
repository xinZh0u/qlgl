/**
 * @file    qldisp_wayland.h
 * @brief   Linux Wayland 窗口显示后端（xdg-shell + wl_shm）。
 *
 * 与 X11 后端的差别：
 *   - Wayland 要求客户端自己提交缓冲区，因此这块驱动有一块**持久帧缓冲**，
 *     每次 flush 把改过的区域拷进当前 shm 缓冲区，再 damage + attach + commit；
 *   - 使用双缓冲：两个 wl_buffer 轮换，全部在用时跳过本帧，避免撕裂。
 *
 * 用法与 X11 后端一致：
 *     QLDispDriver *d = ql_disp_wayland_create("QLGL Demo", 320, 240,
 *                                              QL_COLOR_FORMAT_AUTO);
 *     QLCanvas cv;
 *     ql_canvas_from_disp(&cv, d);
 *     while (ql_disp_wayland_poll(d)) {
 *         ql_input_dispatch();
 *         ql_ui_render_canvas(ui, &cv);
 *     }
 *     ql_disp_wayland_destroy(d);
 *
 * 需要的环境变量：WAYLAND_DISPLAY（通常已由桌面会话设置）、XDG_RUNTIME_DIR。
 */
#ifndef QLGL_DISP_WAYLAND_H
#define QLGL_DISP_WAYLAND_H

#include "qlgl.h"
#include "qldisp.h"

#if defined(QLGL_ENABLE_WAYLAND)

/*
 * 创建窗口。title 可为 NULL（默认 "QLGL"）。
 * 成功后驱动已是活动显示；失败返回 NULL 并打错误日志。
 */
QLDispDriver *ql_disp_wayland_create(const char *title, ql_coord_t width, ql_coord_t height,
                                     QLColorFormat format);

/* 处理挂起事件并返回是否继续：1 继续，0 窗口已关闭 */
int ql_disp_wayland_poll(QLDispDriver *driver);

void ql_disp_wayland_destroy(QLDispDriver *driver);

/* 请求关闭窗口：下一次 poll 返回 0，主循环随之退出 */
void ql_disp_wayland_close(QLDispDriver *driver);

const char *ql_disp_wayland_title(QLDispDriver *driver);

/* 成功提交（commit）的帧数，调试用 */
uint32_t ql_disp_wayland_commit_count(QLDispDriver *driver);

/* 因为两个缓冲区都在用而跳过的帧数 */
uint32_t ql_disp_wayland_skip_count(QLDispDriver *driver);

#endif /* QLGL_ENABLE_WAYLAND */

#endif /* QLGL_DISP_WAYLAND_H */
