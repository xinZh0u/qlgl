/**
 * @file    15_backdrop.c
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

static const char *k_html =
    "<div id=\"root\">"
    /* 下层：四个色块各自来回跑，轨迹故意穿过中间的玻璃卡 */
    "  <div class=\"blob b1\"></div>"
    "  <div class=\"blob b2\"></div>"
    "  <div class=\"blob b3\"></div>"
    "  <div class=\"blob b4\"></div>"
    /* 上层：固定的玻璃卡 */
    "  <div id=\"card\">"
    "    <div class=\"t1\">backdrop-filter: blur(14px)</div>"
    "    <div class=\"t2\">玻璃层背后有 4 个色块在动</div>"
    "  </div>"
    /* 上层：自己也在动的玻璃方块 */
    "  <div id=\"puck\"></div>"
    /* 上层：细横条，小半径模糊 */
    "  <div id=\"pill\">blur(3px)</div>"
    "  <div id=\"hint\">上层固定 + 上层移动 + 下层动态，三条同时成立</div>"
    "</div>";

static const char *k_css =
    /*
     * 根元素直接当舞台：`#root` 自身 `height:100%` 相对视口，是确定高度，
     * 所以它的绝对定位子元素能拿到正确的包含块。
     * （注意：**普通元素**的 `height:100%` 目前不会去解析父元素的百分比高度，
     *   多套一层 `#stage{height:100%}` 会变成 0 高，什么都看不见。）
     */
    "#root { position: relative; width: 100%; height: 100%;"
    "        background-color: #EEF3FB; font-size: 12px; color: #1F2328; }"

    /* ---- 下层：色块。position:absolute + transform 动画，零重排 ---- */
    ".blob { position: absolute; border-radius: 22px; }"
    ".b1 { left: 24px;  top: 40px;  width: 96px; height: 96px;"
    "      background-color: #4C8DFF;"
    "      animation: run1 2600ms ease-in-out infinite alternate; }"
    ".b2 { left: 330px; top: 30px;  width: 84px; height: 84px;"
    "      background-color: #FFB020;"
    "      animation: run2 3200ms ease-in-out infinite alternate; }"
    ".b3 { left: 250px; top: 250px; width: 104px; height: 104px;"
    "      background-color: #34C759;"
    "      animation: run3 2100ms ease-in-out infinite alternate; }"
    ".b4 { left: 60px;  top: 290px; width: 72px; height: 72px;"
    "      background-color: #FF5C7A;"
    "      animation: run4 3800ms ease-in-out infinite alternate; }"

    /* 轨迹：都从右下往左上、或反过来，保证会从 #card 底下穿过 */
    "@keyframes run1 { from { transform: translate(0px, 0px); }"
    "                  to   { transform: translate(190px, 110px); } }"
    "@keyframes run2 { from { transform: translate(0px, 0px); }"
    "                  to   { transform: translate(-170px, 140px); } }"
    "@keyframes run3 { from { transform: translate(0px, 0px); }"
    "                  to   { transform: translate(-140px, -150px); } }"
    "@keyframes run4 { from { transform: translate(0px, 0px); }"
    "                  to   { transform: translate(250px, -110px); } }"

    /* ---- 玻璃卡：半透明底 + 高斯模糊 ---- */
    /*
     * background-color 必须带 alpha。写 #FFFFFF 的话它会糊完再被自己盖回去，
     * 屏幕上就是一整块白 —— 这是使用这个属性最常见的坑。
     */
    "#card { position: absolute; left: 56px; top: 132px; width: 236px; height: 116px;"
    "        background-color: rgba(255, 255, 255, 0.42);"
    "        backdrop-filter: blur(14px);"
    "        border-radius: 16px; border-width: 1px; border-color: rgba(255,255,255,0.85);"
    "        box-shadow: 0px 8px 24px rgba(31, 35, 40, 0.12);"
    "        padding: 16px; }"
    "#card .t1 { font-size: 13px; color: #0B3D91; }"
    "#card .t2 { margin-top: 8px; font-size: 11px; color: #46536B; }"

    /* ---- 会自己动的玻璃方块：模糊区域跟着一起走 ---- */
    "#puck { position: absolute; left: 0px; top: 0px; width: 92px; height: 92px;"
    "        background-color: rgba(255, 255, 255, 0.38);"
    "        backdrop-filter: blur(8px);"
    "        border-radius: 20px; border-width: 1px; border-color: rgba(255,255,255,0.9);"
    "        animation: puck 3000ms ease-in-out infinite alternate; }"
    "@keyframes puck { from { transform: translate(30px, 16px); }"
    "                  to   { transform: translate(340px, 250px); } }"

    /* ---- 细横条：小半径对比 ---- */
    "#pill { position: absolute; left: 56px; top: 262px; width: 150px; height: 30px;"
    "        background-color: rgba(255, 255, 255, 0.5);"
    "        backdrop-filter: blur(3px);"
    "        border-radius: 15px; line-height: 30px; padding: 0px 12px;"
    "        font-size: 11px; color: #46536B; }"

    "#hint { position: absolute; left: 0px; top: 356px; width: 100%; height: 20px;"
    "        line-height: 20px; text-align: center; font-size: 11px; color: #6E7781; }";

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
