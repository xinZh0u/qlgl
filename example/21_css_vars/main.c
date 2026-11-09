/**
 * @file    main.c
 * @brief   CSS 变量：`--x` 与 `var()`，在**构建期**替换成字面量。
 *
 *   ./build/example/example_21_css_vars
 *
 * 这条特性在 qlgl 里特别便宜：样式本来就是编译产物，`--x` 没有"运行期改值"这条路
 * （没有 JS、没有运行期样式表），所以整个特性只是"级联之后再查一次表"。
 * 下游的 `compile_style()` 完全不知道变量存在 —— `QLUIStyle` 一个字节都没变。
 *
 * 想现场看"改一处、处处变"：配合 `example/20_live_reload` 的写法（热重载），
 * 改 `#root` 里的 `--brand` 保存，整个界面立刻换色。
 */
#include "common.h"

#define W 640
#define H 470

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
    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        qlgl_shutdown();
        return 1;
    }
    ql_ui_layout(ui, W, H);

    /*
     * 数一下"变量真的被替换了"：引擎里已经不剩任何 var()，所以这里改成数
     * "同一个主题色出现在多少条样式里" —— 它们应当全部等于 #0969DA 或局部覆盖的紫色。
     */
    int brand_use = 0, purple_use = 0;
    for (uint16_t i = 0; i < doc->style_count; i++) {
        const QLUIStyle *s = &doc->styles[i];
        if (s->text_color == 0x0969DA)
            brand_use++;
        if (s->text_color == 0x8250DF)
            purple_use++;
    }
    printf("节点 %d 个，样式 %d 条：主题色被人用在 %d 条样式上，局部覆盖的紫色 %d 条\n",
           (int)doc->node_count, (int)doc->style_count, brand_use, purple_use);

    const int rc = ex_run(ui, W, H, "QLGL · 21 css vars", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
