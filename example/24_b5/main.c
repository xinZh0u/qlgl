/**
 * @file    main.c
 * @brief   B5 小件：`visibility` / `pointer-events` / `align-self` / `order`。
 *
 *   ./build/example/example_24_b5
 *   ./build/example/example_24_b5 --size 720x520 --dump b5.ppm
 *
 * 点击"可点区域"：装饰层写的是 `pointer-events: none`，所以点击穿过它落到按钮上
 * （终端会打印命中的节点）。把那一行删掉再跑，点击就只会命中装饰层。
 */
#include "common.h"

#define W 720
#define H 520

static void on_click(QLUIRoot *ui, QLUINodeRT *node, void *user)
{
    (void)ui;
    (void)user;
    const QLUITag tag = (node != NULL && node->src != NULL)
                            ? (QLUITag)node->src->tag
                            : (QLUITag)0;
    printf("点击命中：节点 tag=%d，文本=\"%s\"\n", (int)tag,
           (node != NULL && node->text != NULL) ? node->text : "");
}

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
    ql_ui_set_click_cb(ui, on_click, NULL);

    const int rc = ex_run(ui, W, H, "QLGL · 24 B5（点一下紫色按钮）", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
