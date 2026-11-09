/**
 * @file    09_scroll.c
 * @brief   滚动容器：`overflow: auto`。
 *
 *   ./build/example/09_scroll                    # 打开窗口（滚轮 / 拖右侧滚动条）
 *   ./build/example/09_scroll --dump /tmp/s.ppm  # 截图模式
 *   ./build/example/09_scroll --scroll 120       # 命令行直接滚到某处再截
 *
 * 三件事：
 *   1. `overflow: auto` 才滚；`overflow: hidden` 只裁不滚（两者都裁）；
 *   2. 滚动偏移是**烘进布局**的 —— 所以绘制、命中测试、脏区都不用改，
 *      代价是滚动后要重跑一次布局（0.04ms 量级）；
 *   3. 滚动条是覆盖式画的，不占布局空间。
 */
#include "common.h"

#define W 260
#define H 220

static const char *k_html =
    "<div id=\"root\">"
    "  <div class=\"title\">播放列表</div>"
    "  <div class=\"list\">"
    "    <div class=\"row\"><span class=\"n\">01</span><span class=\"t\">晚风心里吹</span></div>"
    "    <div class=\"row\"><span class=\"n\">02</span><span class=\"t\">晴天</span></div>"
    "    <div class=\"row\"><span class=\"n\">03</span><span class=\"t\">红豆</span></div>"
    "    <div class=\"row\"><span class=\"n\">04</span><span class=\"t\">约定</span></div>"
    "    <div class=\"row\"><span class=\"n\">05</span><span class=\"t\">稻香</span></div>"
    "    <div class=\"row\"><span class=\"n\">06</span><span class=\"t\">海底</span></div>"
    "    <div class=\"row\"><span class=\"n\">07</span><span class=\"t\">平凡之路</span></div>"
    "    <div class=\"row\"><span class=\"n\">08</span><span class=\"t\">夜空中最亮的星</span></div>"
    "    <div class=\"row\"><span class=\"n\">09</span><span class=\"t\">后来</span></div>"
    "    <div class=\"row\"><span class=\"n\">10</span><span class=\"t\">小小的太阳</span></div>"
    "  </div>"
    "  <div class=\"hint\">滚轮滚动 / 拖右侧滚动条 / PageDown</div>"
    "</div>";

static const char *k_css =
    /*
     * 页面铺满窗口（width/height 的百分比按视口解析），窗口拉大背景跟着变大。
     * 注意：列表仍是固定高度 —— 本库还不支持 `height: %`，
     * 所以"子元素跟着窗口长高"暂时做不到（见教程 §与 Web 的差别）。
     */
    "#root { width: 100%; height: 100%; box-sizing: border-box; background-color: #FFFFFF;"
    "        padding: 12px; font-size: 12px; color: #1F2328; }"
    ".title { font-size: 14px; color: #0969DA; margin-bottom: 6px; }"

    /* 关键：overflow: auto + 固定高度 → 内容超出就能滚 */
    ".list { width: 100%; height: 140px; overflow: auto;"
    "        background-color: #F6F8FA; border-radius: 8px; }"
    ".row { display: flex; gap: 8px; height: 28px; padding-left: 8px;"
    "       align-items: center; }"
    ".row:hover { background-color: #EEF1F4; }"
    ".n { width: 22px; color: #6E7781; }"
    ".t { color: #1F2328; }"
    ".hint { margin-top: 8px; font-size: 10px; color: #6E7781; }";

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
    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        qlgl_shutdown();
        return 1;
    }
    ql_ui_layout(ui, W, H);

    /* 找出那个滚动容器（.list）：它是唯一 overflow == auto 的节点 */
    QLUINodeRT *list = NULL;
    for (uint16_t i = 0; i < ui->node_count; i++)
        if (ui->nodes[i].style->overflow == QL_UI_OVERFLOW_AUTO) {
            list = &ui->nodes[i];
            break;
        }
    if (list == NULL) {
        printf("没有找到滚动容器\n");
        ql_ui_free(ui);
        ql_ui_doc_free(doc);
        qlgl_shutdown();
        return 1;
    }
    printf("列表可滚 %dpx；每行 %.0fpx，视口 %.0fpx\n",
           (int)list->scroll_max_y,
           (double)(ql_ui_node_rect(&ui->nodes[3]).h),
           (double)ql_ui_node_rect(list).h);

    /* 命令行直接滚一段：方便截图对比 */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--scroll") == 0 && i + 1 < argc) {
            const ql_coord_t y = (ql_coord_t)strtol(argv[i + 1], NULL, 10);
            (void)ql_ui_scroll_to(ui, list, 0, y);
            ql_ui_layout(ui, W, H);
            printf("已滚到 y=%d（夹紧后 %d）\n", (int)y, (int)list->scroll_y);
            break;
        }
    }

    const int rc = ex_run(ui, W, H, "QLGL · 09 scroll", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
