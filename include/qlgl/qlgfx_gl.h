/**
 * @file    qlgfx_gl.h
 * @brief   OpenGL ES 3.0（EGL）GPU 后端。
 *
 * 设计要点：
 *   - 单管线单 draw call：SDF 圆角矩形与字形都走同一个 shader，用 use_tex 分支；
 *   - 顶点先攒在 CPU 侧数组，一帧一次上传，零额外分配；
 *   - 圆角/边框用 SDF 解析计算，不需要纹理图集，天然抗锯齿；
 *   - 字形来自 4bpp 位图字体展开成的 R8 图集；
 *   - 支持"离屏 FBO"模式给测试用（不弹窗），与窗口模式共用同一套绘制代码。
 *
 * 与 CPU 后端的关系：两者都实现 QLDraw2D，输出必须一致（golden image 测试）。
 */
#ifndef QLGL_GFX_GL_H
#define QLGL_GFX_GL_H

#include "qlgl.h"
#include "qldraw2d.h"

#if defined(QLGL_ENABLE_GL)

typedef struct QLGfxGL QLGfxGL;

enum {
    QL_GFX_GL_WINDOW    = 0,   /* 建窗口，可直接看到界面 */
    QL_GFX_GL_OFFSCREEN = 1u << 0,  /* pbuffer 离屏，测试/导出用 */
};

/* 失败返回 NULL（无 EGL / 无 X11 / 无 GLES3 都会失败并打印原因） */
QLGfxGL *ql_gfx_gl_create(const char *title, ql_coord_t width, ql_coord_t height,
                          uint32_t flags);
void     ql_gfx_gl_destroy(QLGfxGL *gfx);

/* 窗口模式：处理事件，返回 1=继续 0=窗口已关闭 */
/*
 * 窗口尺寸变化查询：`ql_gfx_gl_poll()` 里收到 ConfigureNotify 就置位。
 * 取一次即清零；返回 1 时调用方应该按 `ql_gfx_gl_size()` 给的**CSS px** 重新
 * `ql_ui_layout()` —— 重排是界面层的事，后端只负责报告"画布变了"。
 */
int  ql_gfx_gl_resize_pending(QLGfxGL *gfx);
void ql_gfx_gl_size(const QLGfxGL *gfx, ql_coord_t *out_w, ql_coord_t *out_h);

/* 把 X11 剪贴板接到这个界面（Ctrl+C/X/V 生效）。可在窗口建好之后调一次。 */
void ql_gfx_gl_attach_clipboard(QLGfxGL *gfx, QLUIRoot *ui);

int      ql_gfx_gl_poll(QLGfxGL *gfx);
/*
 * 阻塞等到"有事件或超时"（毫秒），返回 1=继续 0=窗口已关闭。
 *
 * 事件驱动的循环在"这一帧没有脏区"时调它，而不是用 sleep 空转：
 * 界面静止时既不渲染也不交换，CPU 占用归零；拖动窗口时也不和合成器抢帧
 * （这正是"缩放掉帧"的主要来源 —— 我们每秒 swap 144 次，合成器还要重配窗口）。
 */
int      ql_gfx_gl_wait(QLGfxGL *gfx, int timeout_ms);

/* 请求关闭窗口：下一次 poll 返回 0，主循环随之退出 */
void     ql_gfx_gl_close(QLGfxGL *gfx);

/*
 * 改缩放（26.6 定点，QL_SCALE_ONE = 1.0）。
 * framebuffer 尺寸仍是设备像素；上层绘制用 CSS px，后端入口乘这个系数。
 * 例：1200×800 的窗口、2x DPR → 传 QL_SCALE_ONE * 2，布局按 600×400 CSS px。
 */
void      ql_gfx_gl_set_scale(QLGfxGL *gfx, ql_coord_t scale_fx);

/* 每帧：begin → 用 ql_gfx_gl_draw() 得到的接口绘制 → end */
void     ql_gfx_gl_begin(QLGfxGL *gfx, ql_color_t clear_color);
void     ql_gfx_gl_end(QLGfxGL *gfx);

/*
 * 保留式脏区（窗口模式，M5.5"动画必须走脏区"红线的实现）。
 *
 * 问题：窗口是双缓冲，`eglSwapBuffers` 之后后缓冲的内容**没有任何保证**——
 * 只重画脏区就等于把其余部分清成了底色（表现为"只动的那块画出来了，别处全没了"）。
 *
 * 做法：开一个和窗口同尺寸的 FBO 当"画布"，动画帧只清空 + 重画脏区，
 * 帧末整块 blit 到窗口。这样局部重绘才是真的局部。
 *
 *   -- 建窗口后调一次 --
 *   ql_gfx_gl_set_retained(gfx, 1);
 *
 *   -- 每一帧 --
 *   ql_gfx_gl_begin_rects(gfx, bg, areas, n);   // n == 0 → 整屏清 + 整屏重画
 *   ql_ui_render_rect(d2d, areas[i]) × n
 *   ql_gfx_gl_end(gfx);
 *   ql_gfx_gl_end(gfx);
 *
 * 关掉（或创建 FBO 失败）时退回"每帧整屏清"，行为与不用它时完全一致。
 */
ql_err_t ql_gfx_gl_set_retained(QLGfxGL *gfx, int on);

/*
 * 只重画 rects 覆盖的区域（与 ql_ui_consume_dirty 的输出同单位）。
 *
 * 返回 **1 表示这一帧必须整屏重画**（首帧 / 窗口改尺寸 / FBO 刚建 / 没开保留模式），
 * 此时调用方应当忽略 rects、直接 `ql_ui_render()`：
 *
 *     int full = ql_gfx_gl_begin_rects(gfx, bg, areas, n);
 *     if (full) ql_ui_render(ui, d2d);
 *     else      for (i < n) ql_ui_render_rect(ui, d2d, areas[i]);
 *     ql_gfx_gl_end(gfx);
 */
int      ql_gfx_gl_begin_rects(QLGfxGL *gfx, ql_color_t clear_color,
                               const QLRect *rects, int count);

QLDraw2D *ql_gfx_gl_draw(QLGfxGL *gfx);

/* 1 = 等垂直同步（默认），0 = 不限帧（基准测试用） */
void     ql_gfx_gl_set_vsync(QLGfxGL *gfx, int on);

/* 阻塞到 GPU 画完（基准测试用；正常渲染不要调用） */
void     ql_gfx_gl_finish(QLGfxGL *gfx);

/* 统计 */
uint32_t ql_gfx_gl_frame_count(QLGfxGL *gfx);
uint32_t ql_gfx_gl_draw_calls(QLGfxGL *gfx);
uint32_t ql_gfx_gl_vertex_count(QLGfxGL *gfx);
/* 上一帧的 begin→end 耗时（微秒；CPU 侧测量） */
uint32_t ql_gfx_gl_frame_us(QLGfxGL *gfx);
/*
 * 上一帧里 `eglSwapBuffers` 占了多久（微秒）。
 * 与 frame_us 一比就知道"这一帧是我们画的慢，还是在等 vsync / 合成器"：
 *     frame_us - swap_us  ≈ 我们自己花的（提交 + 光栅）
 *     swap_us             ≈ 等垂直同步 + 合成器取帧
 */
uint32_t ql_gfx_gl_swap_us(QLGfxGL *gfx);
double   ql_gfx_gl_gpu_mhz(QLGfxGL *gfx);   /* 粗略信息，可能为 0 */

/* 读回像素（RGBA8888，自上而下）。离屏模式必可读；窗口模式读的是后缓冲 */
ql_err_t ql_gfx_gl_read_pixels(QLGfxGL *gfx, uint8_t *rgba, uint32_t stride_bytes);

#endif /* QLGL_ENABLE_GL */

#endif /* QLGL_GFX_GL_H */
