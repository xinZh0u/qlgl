/**
 * @file    example_widget.c
 * @brief   完整示例：搭一个小界面（卡片 + 标题条 + 按钮 + 徽标），渲染并输出 PPM 图片。
 *
 * 运行：
 *   ./build/example_widget                  打开窗口（Wayland/X11），Esc 退出
 *   ./build/example_widget --frames 120     跑 120 帧后退出
 *   ./build/example_widget --mem --ppm a.ppm  无窗口时导出图片
 */
#include <stdio.h>
#include <string.h>

#include <qlgl/qlgl.h>

#include "../window_helper.h"

#if defined(__linux__)
#include <time.h>
extern int nanosleep(const struct timespec *req, struct timespec *rem);
#endif

/* ------------------------- 自定义控件：画一个十字 ------------------------- */
QL_PROPERTY_ENUM_EMPTY(QL_MARK, QLMark, QL_WIDGET, 10)
QL_EVENT_ENUM_EMPTY(QL_MARK, QLMark, QL_WIDGET, 10)

QL_DECLARE(QLMark, ql_mark)

struct QLMark        { QLWidget parent; };
struct QLMarkClass   { QLWidgetClass parent; };
struct QLMarkPrivate { ql_color_t color; ql_coord_t thickness; };

QL_DEFINE_WITH_PRIVATE(QL_MARK, QLMark, ql_mark, ql_widget)

static void ql_mark_draw(QLWidget *self, QLCanvas *canvas)
{
    ql_widget_draw_default(self, canvas);          /* 背景 + 边框 */

    struct QLMarkPrivate *priv = ql_mark_get_private((QLMark *)(void *)self);
    QLRect r = ql_widget_abs_rect((QLObject *)self);
    ql_coord_t cx = r.x + r.w / 2;
    ql_coord_t cy = r.y + r.h / 2;
    ql_coord_t arm = (r.w < r.h ? r.w : r.h) / 2 - 4;

    ql_canvas_draw_line(canvas, cx - arm, cy, cx + arm, cy, priv->thickness, priv->color);
    ql_canvas_draw_line(canvas, cx, cy - arm, cx, cy + arm, priv->thickness, priv->color);
}

static void ql_mark_class_init(QLMarkClass *klass)
{
    ql_widget_class_set_draw(((QLObjectClass *)(void *)klass)->type, ql_mark_draw);
}

static void ql_mark_init(QLMark *self)
{
    struct QLMarkPrivate *priv = ql_mark_get_private(self);
    priv->color = QL_COLOR_WHITE;
    priv->thickness = 3;
}

static void ql_mark_deinit(QLMark *self) { QL_UNUSED(self); }

/* ------------------------- 界面搭建 ------------------------- */
int main(int argc, char **argv)
{
    QLWin win;
    ql_win_parse_args(&win, argc, argv);

    if (qlgl_init() != QL_OK) {
        printf("qlgl_init 失败\n");
        return 1;
    }

    const ql_coord_t W = 320;
    const ql_coord_t H = 200;
    ql_win_load_font();
    if (!ql_win_open(&win, "QLGL Widgets", W, H, QL_COLOR_FORMAT_AUTO))
        return 1;
    QL_UNUSED(win.disp);
    ql_disp_set_bg_color(QL_COLOR_RGB(0x14, 0x18, 0x24));

    /* 根：整屏，深色背景 */
    QLWidget *root = ql_widget_new_ex(0, 0, W, H);
    ql_widget_set_bg((QLObject *)root, QL_COLOR_RGB(0x14, 0x18, 0x24), 255);
    ql_render_set_root((QLObject *)root);
    ql_win_bind_esc(&win, (QLObject *)root);

    /* 卡片：圆角 + 描边 */
    QLWidget *card = ql_widget_new_child((QLObject *)root, 24, 24, W - 48, H - 48);
    ql_widget_set_bg((QLObject *)card, QL_COLOR_RGB(0x24, 0x2C, 0x3C), 255);
    ql_widget_set_border((QLObject *)card, 2, QL_COLOR_RGB(0x46, 0x54, 0x6C), 12);
    ql_object_set_property((QLObject *)card, QL_WIDGET_PROPERTY_CLIP_CHILDREN,
                           ql_value_bool(1));

    /* 标题条 */
    QLWidget *bar = ql_widget_new_child((QLObject *)card, 16, 16, W - 80, 28);
    ql_widget_set_bg((QLObject *)bar, QL_COLOR_SKY, 255);
    ql_widget_set_border((QLObject *)bar, 0, QL_COLOR_SKY, 6);

    /* 按钮：主色 + 圆角 */
    QLWidget *button = ql_widget_new_child((QLObject *)card, 16, 60, 120, 36);
    ql_widget_set_bg((QLObject *)button, QL_COLOR_RGB(0x3C, 0xC0, 0x7A), 255);
    ql_widget_set_border((QLObject *)button, 0, QL_COLOR_WHITE, 8);

    /* 次级按钮：半透明，验证 alpha 混合 */
    QLWidget *ghost = ql_widget_new_child((QLObject *)card, 148, 60, 120, 36);
    ql_widget_set_bg((QLObject *)ghost, QL_COLOR_WHITE, 40);
    ql_widget_set_border((QLObject *)ghost, 1, QL_COLOR_GRAY, 8);

    /* 徽标：自定义控件，跑在按钮右上角 */
    QLType mark_type = QL_REGISTER(ql_mark, QL_WIDGET_TYPE);
    if (mark_type == QL_TYPE_NONE) {
        printf("注册 QLMark 失败\n");
        return 1;
    }
    QLMark *mark = (QLMark *)ql_object_new(mark_type);
    ql_object_set_property((QLObject *)mark, QL_WIDGET_PROPERTY_X, ql_value_int(104));
    ql_object_set_property((QLObject *)mark, QL_WIDGET_PROPERTY_Y, ql_value_int(100));
    ql_object_set_property((QLObject *)mark, QL_WIDGET_PROPERTY_WIDTH, ql_value_int(40));
    ql_object_set_property((QLObject *)mark, QL_WIDGET_PROPERTY_HEIGHT, ql_value_int(40));
    ql_widget_set_bg((QLObject *)mark, QL_COLOR_RGB(0xE8, 0x4C, 0x4C), 255);
    ql_widget_set_border((QLObject *)mark, 0, QL_COLOR_WHITE, 20);
    ql_object_add_child((QLObject *)card, (QLObject *)mark);

    /* 渲染循环：有窗口就持续重绘，否则画一帧导出 */
    long frame = 0;
    while (ql_win_poll(&win)) {
        ql_input_dispatch();              /* 后端入队，这里统一派发 */
        if (frame == 10)
            ql_widget_set_bg((QLObject *)button, QL_COLOR_RGB(0x50, 0xE0, 0x90), 255);
        if (frame == 20)
            ql_widget_set_bg((QLObject *)mark, QL_COLOR_RGB(0x4C, 0x8C, 0xFF), 255);

        int drawn = ql_render();
        if (drawn < 0) {
            printf("ql_render failed: %d\n", drawn);
            break;
        }
        frame++;
        if (win.max_frames > 0 && frame >= win.max_frames)
            break;
        if (win.kind != QL_WIN_KIND_MEM) {
            struct timespec ts = { 0, 16000000L };
            nanosleep(&ts, NULL);
        }
    }
    printf("frames: %ld\n", frame);

    ql_object_unref((QLObject *)root);
    ql_win_close(&win);

    QLMemStats stats;
    ql_mem_stats(&stats);
    printf("内存：分配 %zu 次 / 释放 %zu 次 / 存活 %zu 字节\n",
           stats.alloc_count, stats.free_count, stats.bytes_live);

    qlgl_shutdown();
    return stats.bytes_live == 0 ? 0 : 1;
}
