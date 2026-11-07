/**
 * @file    example_text.c
 * @brief   渲染一张文本样张：两种字号、三种对齐、多行、UTF-8 缺字兜底。
 *
 * 运行：./build/example_text [输出路径.ppm]
 */
#include <stdio.h>

#include <qlgl/qlgl.h>

#define W 420
#define H 230


int main(int argc, char **argv)
{
    const char *out = (argc > 1) ? argv[1] : "/tmp/qlgl_example_text.ppm";

    if (qlgl_init() != QL_OK) {
        printf("qlgl_init 失败\n");
        return 1;
    }

    QLDispDriver *disp = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
    if (disp == NULL)
        return 1;
    ql_disp_set_bg_color(QL_COLOR_RGB(0x10, 0x14, 0x1C));

    QLWidget *root = ql_widget_new_ex(0, 0, W, H);
    ql_widget_set_bg((QLObject *)root, QL_COLOR_RGB(0x10, 0x14, 0x1C), 255);
    ql_render_set_root((QLObject *)root);

    /* 卡片 */
    QLWidget *card = ql_widget_new_child((QLObject *)root, 12, 12, W - 24, H - 24);
    ql_widget_set_bg((QLObject *)card, QL_COLOR_RGB(0x1C, 0x23, 0x30), 255);
    ql_widget_set_border((QLObject *)card, 1, QL_COLOR_RGB(0x3A, 0x48, 0x60), 10);

    /* 标题：16 号字 */
    QLLabel *title = ql_label_new_child((QLObject *)card, "QLGL 文本系统 / Text", 16, 14);
    ql_label_set_font((QLObject *)title, &ql_font_mono_16);
    ql_label_set_text_color((QLObject *)title, QL_COLOR_RGB(0x6C, 0xC8, 0xFF));

    /* 副标题：12 号字 */
    QLLabel *sub = ql_label_new_child((QLObject *)card, "mono_12: The quick brown fox jumps over 1234567890", 16, 44);
    ql_label_set_text_color((QLObject *)sub, QL_COLOR_RGB(0xC8, 0xD2, 0xE0));

    /* 多行 */
    QLLabel *multi = ql_label_new_child((QLObject *)card, "line one\nline two\nline three", 16, 70);
    ql_label_set_text_color((QLObject *)multi, QL_COLOR_RGB(0x9A, 0xA8, 0xBC));

    /* 三种对齐：三个同宽标签 */
    static const char *names[] = { "left", "center", "right" };
    static const QLTextAlign aligns[] = {
        QL_TEXT_ALIGN_LEFT, QL_TEXT_ALIGN_CENTER, QL_TEXT_ALIGN_RIGHT,
    };
    for (int i = 0; i < 3; i++) {
        QLLabel *l = ql_label_new_child((QLObject *)card, names[i], 16, 136 + i * 20);
        ql_object_set_property((QLObject *)l, QL_LABEL_PROPERTY_AUTO_SIZE,
                               ql_value_bool(0));
        ql_object_set_property((QLObject *)l, QL_WIDGET_PROPERTY_WIDTH,
                               ql_value_int(160));
        ql_object_set_property((QLObject *)l, QL_WIDGET_PROPERTY_HEIGHT,
                               ql_value_int(ql_font_mono_12.line_height));
        ql_label_set_align((QLObject *)l, aligns[i]);
        ql_label_set_text_color((QLObject *)l, QL_COLOR_RGB(0xF0, 0xB0, 0x30));
    }

    /* 缺字兜底 + 彩色 */
    QLLabel *cjk = ql_label_new_child((QLObject *)card, "中文需要字体支持：你好", 200, 140);
    ql_label_set_text_color((QLObject *)cjk, QL_COLOR_RGB(0xE8, 0x4C, 0x4C));

    /* 带背景的标签 */
    QLLabel *badge = ql_label_new_child((QLObject *)card, " label ", 200, 170);
    ql_widget_set_bg((QLObject *)badge, QL_COLOR_RGB(0x3C, 0xC0, 0x7A), 255);
    ql_widget_set_border((QLObject *)badge, 0, QL_COLOR_WHITE, 8);
    ql_label_set_text_color((QLObject *)badge, QL_COLOR_BLACK);

    int drawn = ql_render();
    printf("绘制 %d 个脏区\n", drawn);
    printf("标题尺寸 %dx%d，字体 %s\n",
           (int)ql_widget_width((QLObject *)title),
           (int)ql_widget_height((QLObject *)title),
           ql_label_font((QLObject *)title)->name);

    if (ql_disp_mem_save_ppm(disp, out) == QL_OK)
        printf("已写出 %s\n", out);

    ql_object_unref((QLObject *)root);
    ql_disp_mem_destroy(disp);

    QLMemStats stats;
    ql_mem_stats(&stats);
    printf("内存：分配 %zu / 释放 %zu / 存活 %zu 字节\n",
           stats.alloc_count, stats.free_count, stats.bytes_live);

    qlgl_shutdown();
    return stats.bytes_live == 0 ? 0 : 1;
}
