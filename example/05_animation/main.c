/**
 * @file    main.c
 * @brief   动画：transition 与 @keyframes。
 *
 *   ./build/example/example_05_animation --frames 200
 *
 * 两类动画的区别：
 *   - `transition`   "状态变了就渐变过去"，由交互触发；
 *   - `@keyframes`   "自己一直播"，与交互无关。
 * 两者都**只改绘制期属性**，不进布局、不重排；每帧只重绘脏区。
 *
 * 主循环里唯一要做的事：
 *     ql_ui_anim_tick(ui, ql_anim_now_ms());   // 返回 0 表示没有东西在动
 */
#include "common.h"

int main(int argc, char **argv)
{
    if (qlgl_init() != QL_OK)
        return 1;
    ex_setup_locale();
    ex_load_font();

    QLUIDoc *doc = ex_load_ui("index.html", "style.css");
    if (doc == NULL) {
        printf("界面编译失败：%s\n", ql_ui_text_last_error());
        qlgl_shutdown();
        return 1;
    }
    printf("%u 段动画 / %u 个关键帧\n", (unsigned)doc->anim_count,
           (unsigned)doc->keyframe_count);

    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        qlgl_shutdown();
        return 1;
    }
    ql_ui_layout(ui, 300, 160);

    /* 逐帧推进，打印"这一帧有几块脏区"——动画只在脏区里重绘 */
    for (int t = 0; t <= 700; t += 100) {
        const uint32_t moving = ql_ui_anim_tick(ui, (uint32_t)t);
        QLRect areas[16];
        const int n = ql_ui_consume_dirty(ui, areas, 16);
        printf("t=%3dms  在动 %u 个  脏区 %d 块\n", t, (unsigned)moving, n);
    }

    const int rc = ex_run(ui, 300, 160, "QLGL · 05 animation", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
