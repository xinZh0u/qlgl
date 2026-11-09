/**
 * @file    main.c
 * @brief   输入控件：`<input>` 与 `<select>`。
 *
 *   ./build/example/example_10_input                    # 打开窗口，直接打字
 *   ./build/example/example_10_input --dump /tmp/a.ppm  # 截图模式
 *   ./build/example/example_10_input --fill             # 直接填好再截（看效果）
 *
 * 四件事：
 *   1. `<input>` 的值存在运行期池里（不是编译期静态数据），是本库唯一的可变文本；
 *   2. 键盘、光标、选区都是 `ql_ui_input_event()` 驱动的 —— 平台层只负责把
 *      操作系统的事件翻译成 `QLInputEvent`；
 *   3. `<option>` 是 `display: none`，只活在下拉面板里，不进布局；
 *   4. 面板是**覆盖层**：不占空间、不裁父级 —— 和浏览器 popup 一样。
 */
#include "common.h"

#define W 300
#define H 260

/* 回调里能拿到值：回车或点选后打印一行，方便在终端里看 */
static void on_change(QLUIRoot *ui, QLUINodeRT *node, const char *value, void *user)
{
    (void)ui;
    (void)node;
    (void)user;
    printf("  [事件] %s\n", value);
}

/* 按标签找节点（例程里用不上 id 选择器那套，直接扫一遍最省事） */
static QLUINodeRT *find(QLUIRoot *ui, uint16_t tag)
{
    for (uint16_t i = 0; i < ui->node_count; i++)
        if (ui->nodes[i].src->tag == tag)
            return &ui->nodes[i];
    return NULL;
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
    ql_ui_set_input_cb(ui, on_change, NULL);
    ql_ui_set_select_cb(ui, on_change, NULL);

    QLUINodeRT *user = find(ui, QL_UI_TAG_INPUT);
    QLUINodeRT *role = find(ui, QL_UI_TAG_SELECT);
    printf("账号 = \"%s\"，角色 = \"%s\"（第 %d 项）\n",
           ql_ui_input_value(ui, user),
           role != NULL && role->text != NULL ? role->text : "",
           role != NULL ? ql_ui_select_index(ui, role) : -1);

    /* --fill：把值改掉再截，说明"设备上报的真实值"是这么进来的 */
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--fill") == 0 && role != NULL) {
            (void)ql_ui_input_set_value(ui, user, "张三");
            (void)ql_ui_select_set_index(ui, role, 2);
            ql_ui_layout(ui, W, H);
            printf("已改成 账号 = \"%s\"，角色 = \"%s\"\n",
                   ql_ui_input_value(ui, user), role->text);
            break;
        }

    const int rc = ex_run(ui, W, H, "QLGL · 10 input", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
