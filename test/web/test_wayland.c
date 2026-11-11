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
#endif

QL_TEST_BEGIN("test_wayland")

#if !defined(QLGL_ENABLE_WAYLAND)
    printf("  这个构建没有 Wayland 后端，跳过\n");
#else
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
#endif

QL_TEST_END()
