/* setenv/unsetenv 的声明要 _GNU_SOURCE，且必须在任何头文件之前 */
#if defined(__linux__)
#define _GNU_SOURCE 1
#endif

/**
 * @file    test_wayland.c
 * @brief   Wayland 后端：高 DPI（`wl_output.scale`）与帧缓冲尺寸的不变量。
 *
 * 这个测试需要一个**真的合成器**（`WAYLAND_DISPLAY`），所以默认不跑：
 *
 *     QLGL_TEST_WAYLAND=1 ./build/test_wayland
 *
 * CI 上没有合成器，把它当"可选冒烟"而不是常驻门禁 —— 但本机开发时它是
 * 唯一能自动验证"2x 屏上缓冲区该多大、CSS 坐标怎么映射"的手段，
 * 值得留着手动跑。
 *
 * 验的是**不变量**（不依赖某块屏的具体分辨率）：
 *   1. `base.width/height` 永远是**设备像素** = CSS × scale，stride = 宽 × 4；
 *   2. `ql_disp_css_width()` 永远等于创建时给的逻辑尺寸（布局基准不随缩放变）；
 *   3. 画布跟着驱动走：2x 下画 10 CSS px 的方块要覆盖 20 个设备像素；
 *   4. 销毁后没有内存泄漏（`QL_TEST_END` 统一查 `bytes_live`）。
 */
#include <stdlib.h>

#include "../ql_test.h"

#if defined(QLGL_ENABLE_WAYLAND)
#include <qlgl/qldisp_wayland.h>
/*
 * 窗口外壳本身也拉进来：要检查**监听器槽位一个不漏**。
 * libwayland 收到事件而槽位为 NULL 时会直接 `abort()`（程序"一闪就没了"），
 * 而合成器按我们 `bind` 的版本发事件 —— 版本和槽位必须对得上。
 */
#include "../../src/port/qlwindow_wayland.h"
#endif
#if defined(QLGL_ENABLE_GL)
#include <qlgl/qlgfx_gl.h>
#endif

QL_TEST_BEGIN("test_wayland")

#if !defined(QLGL_ENABLE_WAYLAND)
    printf("  这个构建没有 Wayland 后端，跳过\n");
#else
    /*
     * ---------- 0. 监听器完整性（不需要合成器，离线也能查） ----------
     *
     * `qlwl_registry_global()` 把 `wl_seat` 绑到 v5：指针就会有 frame /
     * axis_source / axis_stop / axis_discrete 四个事件。少任何一个槽位，
     * **鼠标进窗口那一刻进程就 abort**（现象：窗口一闪就没了 / 根本没显示）。
     */
    QL_CHECK(qlwl_ptr_listener.enter != NULL);
    QL_CHECK(qlwl_ptr_listener.leave != NULL);
    QL_CHECK(qlwl_ptr_listener.motion != NULL);
    QL_CHECK(qlwl_ptr_listener.button != NULL);
    QL_CHECK(qlwl_ptr_listener.axis != NULL);
    QL_CHECK(qlwl_ptr_listener.frame != NULL);              /* v5 */
    QL_CHECK(qlwl_ptr_listener.axis_source != NULL);        /* v5 */
    QL_CHECK(qlwl_ptr_listener.axis_stop != NULL);          /* v5 */
    QL_CHECK(qlwl_ptr_listener.axis_discrete != NULL);      /* v5 */
    QL_CHECK(qlwl_kb_listener.keymap != NULL);
    QL_CHECK(qlwl_kb_listener.enter != NULL);
    QL_CHECK(qlwl_kb_listener.leave != NULL);
    QL_CHECK(qlwl_kb_listener.key != NULL);
    QL_CHECK(qlwl_kb_listener.modifiers != NULL);
    QL_CHECK(qlwl_kb_listener.repeat_info != NULL);         /* v4 */
    QL_CHECK(qlwl_touch_listener.down != NULL);
    QL_CHECK(qlwl_touch_listener.up != NULL);
    QL_CHECK(qlwl_touch_listener.motion != NULL);
    QL_CHECK(qlwl_touch_listener.frame != NULL);
    QL_CHECK(qlwl_touch_listener.cancel != NULL);
    QL_CHECK(qlwl_seat_listener.capabilities != NULL);
    QL_CHECK(qlwl_seat_listener.name != NULL);              /* v2 */
    QL_CHECK(qlwl_wm_listener.ping != NULL);
    QL_CHECK(qlwl_xdg_listener.configure != NULL);
    QL_CHECK(qlwl_toplevel_listener.configure != NULL);
    QL_CHECK(qlwl_toplevel_listener.close != NULL);
    /* 绑定 xdg_wm_base v1 / wl_output v2 / wl_shm v1：别多实现也别少实现 */
    /*
     * ---------- 0b. `xdg_toplevel.configure` 的尺寸必须被采纳 ----------
     *
     * 不采纳的表现：用户拖动窗口边缘时合成器把**旧尺寸的缓冲区拉伸**到新窗口，
     * 内容不重排、字变糊（点对点变成了插值放大）。这里用纯逻辑验一遍：
     *   - 0/0 表示"你自己定"（首帧就是这样）→ 不能当成 0 尺寸；
     *   - 给了尺寸 → 记下来，且**只取一次**；
     *   - 和当前尺寸相同 → 不算变化（免得每帧重排）。
     */
    {
        QLWlWindow w;
        memset(&w, 0, sizeof(w));
        w.css_w = 340;
        w.css_h = 400;

        struct wl_array empty;
        wl_array_init(&empty);
        qlwl_toplevel_configure(&w, NULL, 0, 0, &empty);      /* 你自己定 */
        int32_t cw = 0, ch = 0;
        QL_CHECK_EQ(qlwl_take_resize(&w, &cw, &ch), 0);
        QL_CHECK_EQ((int)w.css_w, 340);

        qlwl_toplevel_configure(&w, NULL, 500, 300, &empty);  /* 用户拖大了 */
        QL_CHECK_EQ((int)w.resize_pending, 1);
        QL_CHECK_EQ(qlwl_take_resize(&w, &cw, &ch), 1);
        QL_CHECK_EQ((int)cw, 500);
        QL_CHECK_EQ((int)ch, 300);
        QL_CHECK_EQ((int)w.css_w, 500);
        QL_CHECK_EQ(qlwl_take_resize(&w, &cw, &ch), 0);       /* 取过一次就没了 */

        qlwl_toplevel_configure(&w, NULL, 500, 300, &empty);  /* 同尺寸：不算变化 */
        QL_CHECK_EQ(qlwl_take_resize(&w, &cw, &ch), 0);
        wl_array_release(&empty);
    }

    QL_CHECK(qlwl_out_listener.geometry != NULL);
    QL_CHECK(qlwl_out_listener.mode != NULL);
    QL_CHECK(qlwl_out_listener.done != NULL);
    QL_CHECK(qlwl_out_listener.scale != NULL);

    const char *opt = getenv("QLGL_TEST_WAYLAND");
    if (opt == NULL || opt[0] == '\0') {
        printf("  需要合成器：设置 QLGL_TEST_WAYLAND=1 才跑（当前跳过）\n");
    } else if (getenv("WAYLAND_DISPLAY") == NULL) {
        printf("  WAYLAND_DISPLAY 为空：没有合成器，跳过\n");
    } else {
        for (int pass = 0; pass < 2; pass++) {
            const int want_scale = (pass == 0) ? 1 : 2;
            /* 驱动在 create 时读这个环境变量（覆盖合成器上报的 scale） */
            setenv("QLGL_WAYLAND_SCALE", (pass == 0) ? "1" : "2", 1);

            const ql_coord_t css_w = 100, css_h = 60;
            QLDispDriver *d = ql_disp_wayland_create("QLGL wayland test", css_w,
                                                     css_h, QL_COLOR_FORMAT_AUTO);
            QL_CHECK(d != NULL);
            if (d == NULL)
                continue;

            QL_CHECK_EQ(ql_disp_wayland_scale(d), want_scale);
            /* 设备像素 = CSS × scale */
            QL_CHECK_EQ((int)d->width, (int)(css_w * want_scale));
            QL_CHECK_EQ((int)d->height, (int)(css_h * want_scale));
            QL_CHECK_EQ((int)d->stride, (int)(d->width * 4));
            QL_CHECK_EQ((int)d->format, (int)QL_COLOR_ARGB8888);
            /* CSS 尺寸不随缩放变（布局基准） */
            QL_CHECK_EQ((int)ql_disp_css_width(d), (int)css_w);
            QL_CHECK_EQ((int)ql_disp_css_height(d), (int)css_h);

            QLCanvas cv;
            QL_CHECK_EQ(ql_canvas_from_disp(&cv, d), QL_OK);
            QL_CHECK_EQ((int)cv.width, (int)d->width);
            QL_CHECK_EQ((int)cv.scale_fx, (int)d->scale_fx);

            /* 10 CSS px 的方块 → 10×scale 设备像素 */
            QLUIDoc *doc = ql_ui_load_text("<div id=a></div>",
                                           "#a { width: 10px; height: 10px; "
                                           "background: #ff0000 }");
            QL_CHECK(doc != NULL);
            if (doc != NULL) {
                QLUIRoot *ui = ql_ui_build(doc);
                QL_CHECK(ui != NULL);
                if (ui != NULL) {
                    ql_ui_layout(ui, ql_disp_css_width(d), ql_disp_css_height(d));
                    ql_canvas_clear(&cv, 0xFFFFFF);
                    ql_ui_render_canvas(ui, &cv);

                    const int edge = 10 * want_scale;
                    QL_CHECK_EQ((int)(ql_canvas_read_pixel(&cv, 0, 0) & 0xFFFFFFu),
                                0xFF0000);
                    QL_CHECK_EQ((int)(ql_canvas_read_pixel(&cv, edge - 1, 0) & 0xFFFFFFu),
                                0xFF0000);
                    QL_CHECK_EQ((int)(ql_canvas_read_pixel(&cv, edge, 0) & 0xFFFFFFu),
                                0xFFFFFF);
                    QL_CHECK_EQ((int)(ql_canvas_read_pixel(&cv, 0, edge - 1) & 0xFFFFFFu),
                                0xFF0000);
                    ql_ui_free(ui);
                }
                ql_ui_doc_free(doc);
            }

            /* 推两帧，确认提交路径没崩（合成器在不在都该安全） */
            d->flush(d, ql_rect_make(0, 0, d->width, d->height));
            d->flush_done(d);
            (void)ql_disp_wayland_poll(d);
            QL_CHECK(ql_disp_wayland_commit_count(d) >= 1);

            ql_disp_wayland_destroy(d);
            unsetenv("QLGL_WAYLAND_SCALE");
        }
    }

#if defined(QLGL_ENABLE_GL) && defined(QLGL_HAVE_WAYLAND_EGL)
    /*
     * GPU 后端（`wl_egl_window`）：同样验"设备像素 = CSS × scale"这条不变量，
     * 另外验**真的画对了**——读回帧缓冲，红方块必须正好覆盖 10×scale 个设备像素。
     *
     * 第二个断言是关键：`g->width` 曾经只被赋成 CSS 尺寸（缩放只乘了局部变量），
     * 于是 viewport 只有四分之一大，2x 下画面挤在左上角。只查尺寸不查像素
     * 是发现不了的。
     */
    if (getenv("QLGL_TEST_WAYLAND") != NULL && getenv("WAYLAND_DISPLAY") != NULL) {
        setenv("QLGL_GL_BACKEND", "wayland", 1);
        const char *be_opt = getenv("QLGL_TEST_WAYLAND");
        if (be_opt != NULL && be_opt[0] != '\0') {
            for (int pass = 0; pass < 2; pass++) {
                const int want_scale = (pass == 0) ? 1 : 2;
                setenv("QLGL_WAYLAND_SCALE", (pass == 0) ? "1" : "2", 1);

                const ql_coord_t css_w = 100, css_h = 60;
                QLGfxGL *g = ql_gfx_gl_create("QLGL gl wayland test", css_w, css_h, 0);
                QL_CHECK(g != NULL);
                if (g == NULL)
                    continue;

                ql_coord_t sw = 0, sh = 0;
                ql_gfx_gl_size(g, &sw, &sh);
                QL_CHECK_EQ((int)sw, (int)css_w);        /* CSS 尺寸不随缩放变 */
                QL_CHECK_EQ((int)sh, (int)css_h);
                ql_gfx_gl_device_size(g, &sw, &sh);
                QL_CHECK_EQ((int)sw, (int)(css_w * want_scale));
                QL_CHECK_EQ((int)sh, (int)(css_h * want_scale));

                QL_CHECK_EQ(ql_gfx_gl_poll(g), 1);       /* 泵一轮事件不崩 */

                /* body 默认有 8px 外边距，去掉它方块才正好贴在 (0,0) */
                QLUIDoc *doc = ql_ui_load_text("<div id=a></div>",
                                               "body { margin: 0 } "
                                               "#a { width: 10px; height: 10px; "
                                               "background: #ff0000 }");
                QL_CHECK(doc != NULL);
                if (doc != NULL) {
                    QLUIRoot *ui = ql_ui_build(doc);
                    QL_CHECK(ui != NULL);
                    if (ui != NULL) {
                        ql_ui_layout(ui, css_w, css_h);
                        QL_CHECK_EQ(ql_gfx_gl_begin_rects(g, 0xFFFFFF, NULL, 0), 1);
                        ql_ui_render(ui, ql_gfx_gl_draw(g));
                        /* 先落盘（顶点批次在这里才上传）、不交换，然后读回 */
                        ql_gfx_gl_flush_frame(g);

                        /* swap 之前读：交换之后后缓冲内容未定义（Wayland 上是黑的） */
                        ql_gfx_gl_finish(g);
                        uint8_t *px = (uint8_t *)calloc(1, (size_t)sw * (size_t)sh * 4u);
                        QL_CHECK(px != NULL);
                        if (px != NULL) {
                            QL_CHECK_EQ(ql_gfx_gl_read_pixels(g, px, (uint32_t)sw * 4u),
                                        QL_OK);
                            const int edge = 10 * want_scale;
                            const uint8_t *p00 = px;     /* (0,0) 在方块里 */
                            const uint8_t *pin = px + (size_t)(edge - 1) * 4u;
                            const uint8_t *pout = px + (size_t)edge * 4u;

                            QL_CHECK(p00[0] > 240 && p00[1] < 16 && p00[2] < 16);
                            QL_CHECK(pin[0] > 240 && pin[1] < 16 && pin[2] < 16);
                            QL_CHECK(pout[0] > 240 && pout[1] > 240 && pout[2] > 240);
                            free(px);
                        }
                        ql_gfx_gl_end(g);
                        ql_ui_free(ui);
                    }
                    ql_ui_doc_free(doc);
                }

                QL_CHECK_EQ(ql_gfx_gl_poll(g), 1);       /* 交换之后再泵一轮 */
                QL_CHECK(ql_gfx_gl_frame_count(g) >= 1);
                ql_gfx_gl_destroy(g);
                unsetenv("QLGL_WAYLAND_SCALE");
            }
        }
        unsetenv("QLGL_GL_BACKEND");
    }
#endif
#endif

QL_TEST_END()
