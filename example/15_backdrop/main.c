/**
 * @file    main.c
 * @brief   毛玻璃叠在**动的**背景上：上层半透明 + 高斯模糊，下层实时在动。
 *
 *   ./build/example/example_15_backdrop
 *   ./build/example/example_15_backdrop --size 320x280   # 看小窗口
 *
 * 一张图说清三件事：
 *
 *   1. `backdrop-filter: blur(Npx)` 模糊的是**这一层背后已经画好的内容**，
 *      所以它天然跟随 z 序 —— 写在后面的元素才糊得到前面的。
 *   2. 玻璃层自己的背景**必须是半透明的**（`rgba(255,255,255,0.42)`）。
 *      写成不透明的 `#FFFFFF` 会把模糊结果整个盖住，看上去像"模糊没生效"。
 *   3. 下层的运动走 `@keyframes` + `transform: translate()`：动画零重排，
 *      每帧只有脏区重画，玻璃区跟着重新取"背后的像素"再糊一次 —— 实时跟随。
 *
 * 画面里三块玻璃：
 *
 *   #card   固定不动，底下的色块从它下面穿过（最容易看出是否实时）
 *   #puck   自己也在移动，模糊区域跟着它走
 *   #pill   一条细横条，半径小，用来对比 blur(3px) 与 blur(14px) 的观感
 *
 * 没有 JS：所有动态都是 CSS 动画。
 */
#include "common.h"

#include <qlgl/qlgl.h>

#define W 480
#define H 400

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

    /* 数一下有几块玻璃：这是构建期就定下来的，运行期不再判断 */
    int glass = 0;
    for (uint16_t i = 0; i < ui->node_count; i++)
        if (ql_ui_node_style(&ui->nodes[i])->backdrop_blur > 0)
            glass++;
    printf("节点 %d 个，其中带 backdrop-filter 的 %d 个\n",
           (int)ui->node_count, glass);

    const int rc = ex_run(ui, W, H, "QLGL · 15 backdrop", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
