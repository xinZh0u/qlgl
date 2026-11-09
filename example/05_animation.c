/**
 * @file    05_animation.c
 * @brief   动画：transition 与 @keyframes。
 *
 *   ./build/example/05_animation --frames 200
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

static const char *k_html =
    "<div id=\"root\">"
    "  <div class=\"slide\">滑入</div>"
    "  <div class=\"pulse\">脉冲</div>"
    "  <div class=\"flip\">翻转</div>"
    "  <div class=\"hint\">平移 · 颜色循环 · scaleX 360° 翻转</div>"
    "</div>";

static const char *k_css =
    "#root { width: 100%; height: 100%; box-sizing: border-box; background-color: #FFFFFF; }"
    ".slide, .pulse, .flip { position: absolute; top: 40px; width: 76px;"
    "  height: 56px; border-radius: 8px; font-size: 13px; text-align: center;"
    "  line-height: 56px;"
    "  color: #10151C; }"

    ".slide { left: 16px; background-color: #E8743B;"
    "         animation: slide-in 1400ms ease-out infinite; }"
    ".pulse { left: 112px; background-color: #2FB8A6;"
    "         animation: pulse 1400ms ease-in-out infinite; }"
    ".flip  { left: 208px; background-color: #7A5AF8;"
    "         animation: flip-x 2800ms linear infinite; }"

    ".hint { position: absolute; left: 16px; top: 112px; font-size: 11px;"
    "        color: #6E7781; }"

    "@keyframes slide-in {"
    "  from { transform: translate(-24px, 0px); }"
    "  to   { transform: translate(0px, 0px); }"
    "}"
    "@keyframes pulse {"
    "  0%   { background-color: #2FB8A6; }"
    "  50%  { background-color: #E6C34A; }"
    "  100% { background-color: #2FB8A6; }"
    "}"
    /*
     * 真正的 360° 翻转：1 → 0 → -1 → 0 → 1。
     * 负值的半程是**镜像**（字反着写），高度全程不变。
     */
    "@keyframes flip-x {"
    "  0%   { transform: scaleX(1); }"
    "  25%  { transform: scaleX(0); }"
    "  50%  { transform: scaleX(-1); }"
    "  75%  { transform: scaleX(0); }"
    "  100% { transform: scaleX(1); }"
    "}";

int main(int argc, char **argv)
{
    if (qlgl_init() != QL_OK)
        return 1;
    ex_setup_locale();
    ex_load_font();

    QLUIDoc *doc = ql_ui_load_text(k_html, k_css);
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
