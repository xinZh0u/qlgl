/**
 * @file    14_css3.c
 * @brief   CSS3 补上的那几件"真实页面天天用"的东西。
 *
 *   ./build/example/example_14_css3
 *
 * 这一版补的是（对应《CSS3 覆盖与裁剪》的 B2 + B1 第一片）：
 *
 *   ::before / content   图标、装饰、前缀 —— 构建期展开成真节点
 *   :nth-child(2n)       斑马纹，不用手工加 class
 *   [attr] / [attr=v]    按数据属性出样式
 *   :not()               排除某几条
 *   :first-child         首行/末行特殊处理
 *   min-width/max-width  列表卡片在窄窗口里不再被压扁，宽窗口里不再拉太长
 *   !important           兜底覆盖
 *   hsl()                配色不用手算十六进制
 *
 * 注意 **没有 JS**：所有交互都是 CSS 状态样式 + @keyframes。
 */
#include "common.h"

#define W 420
#define H 460

static const char *k_html =
    "<div id=\"root\">"
    "  <h2>版本历史</h2>"
    /* ::before 生成一个圆点前缀，:first-child 给首条加高亮 */
    "  <div id=\"list\">"
    "    <div class=\"row\" data-state=\"ok\">   <span>v0.3 布局与 flexbox</span></div>"
    "    <div class=\"row\" data-state=\"ok\">   <span>v0.4 动效与毛玻璃</span></div>"
    "    <div class=\"row\" data-state=\"warn\"> <span>v0.5 图像管线</span></div>"
    "    <div class=\"row\" data-state=\"ok\">   <span>v0.6 HTML5 全覆盖</span></div>"
    "    <div class=\"row\" data-state=\"ok\">   <span>v0.7 CSS3 补全</span></div>"
    "  </div>"
    "  <div id=\"tags\">"
    "    <span class=\"tag\" data-kind=\"new\">new</span>"
    "    <span class=\"tag\" data-kind=\"fix\">fix</span>"
    "    <span class=\"tag\">doc</span>"
    "  </div>"
    "  <div id=\"bar\"><div id=\"fill\"></div></div>"
    "  <p class=\"hint\">拖窗口宽度看 max-width / min-width 的效果</p>"
    "</div>";

static const char *k_css =
    "#root { width: 100%; height: 100%; box-sizing: border-box;"
    "        background-color: #FFFFFF; padding: 14px;"
    "        font-size: 12px; color: #1F2328; }"
    "h2 { font-size: 16px; color: #0969DA; margin: 0 0 10px 0; }"

    /* ---- 列表：斑马纹 + ::before 圆点 + 首/末条特殊 ---- */
    "#list { width: 100%; border-radius: 8px; background-color: #F6F8FA; }"
    ".row { height: 26px; padding: 0px 10px; line-height: 26px;"
    "       border-width: 1px; border-color: #E4E8ED; }"
    /* 偶数行换底色：不用手工加 class，`nth-child` 构建期就结算成具体样式 */
    ".row:nth-child(2n) { background-color: #EEF2F7; }"
    ".row:last-child { border-color: #F6F8FA; }"   /* 末条不画分隔线 */
    /* ::before 生成前缀圆点（构建期变成真节点，运行期零成本） */
    ".row::before { content: \"• \"; color: #8B949E; }"
    /* 首条特殊：整行强调 */
    ".row:first-child { color: #0969DA; }"
    ".row:first-child::before { content: \"★ \"; color: #E6C34A; }"
    /* 属性选择器：按 data-state 出颜色 */
    ".row[data-state=warn] { color: #9A6700; }"
    ".row[data-state=warn]::before { content: \"! \"; color: #BF8700; }"

    /* ---- 标签：只用 :not() 排除最后一条的右边距 ---- */
    "#tags { margin: 10px 0px 0px 0px; }"
    ".tag { display: inline-block; padding: 2px 8px; margin-right: 6px;"
    "       border-radius: 10px; background-color: #EAF2FF; color: #0969DA;"
    "       font-size: 10px; }"
    /* `:not()`：除了带 data-kind 的，其余降低存在感 */
    ".tag:not([data-kind]) { color: #57606A; background-color: #F0F2F5; }"
    ".tag[data-kind=new] { background-color: #DFF5E9; color: #1A7F37; }"
    ".tag[data-kind=fix] { background-color: #FFF1E5; color: #9A6700; }"

    /* ---- 进度：hsl() 配色 + min/max 宽度 ---- */
    "#bar { width: 100%; height: 10px; margin-top: 12px; border-radius: 5px;"
    "       background-color: #E4E8ED;"
    "       min-width: 120px; max-width: 260px; }"
    "#fill { width: 62%; height: 10px; border-radius: 5px;"
    "        background-color: hsl(210, 90%, 55%);"
    "        animation: pulse 1600ms ease-in-out infinite alternate; }"
    /*
     * 关键帧里**只能改绘制属性**（颜色/透明度/transform/clip-path），不能改 width ——
     * 改宽度意味着每帧重排，与本库"动画零重排"的红线冲突。
     */
    "@keyframes pulse { from { background-color: hsl(210, 90%, 55%); }"
    "                   to   { background-color: hsl(150, 70%, 42%); } }"

    ".hint { margin-top: 12px; font-size: 10px; color: #6E7781;"
    "        /* !important 兜底：别的规则再想改它的颜色也改不动 */"
    "        color: #6E7781 !important; }";

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

    /*
     * 看一眼"构建期展开"的结果：`::before` 的圆点/星号已经变成真节点，
     * 所以节点数比 HTML 里写的元素多。这就是"零运行期成本"的证据。
     */
    int pseudo = 0;
    for (uint16_t i = 0; i < ui->node_count; i++)
        if (ql_ui_node_text(&ui->nodes[i]) != NULL &&
            (unsigned char)ql_ui_node_text(&ui->nodes[i])[0] == 0xE2)  /* "•" 首字节 */
            pseudo++;
    printf("节点 %d 个（其中 %d 个是 ::before 生成的）\n",
           (int)ui->node_count, pseudo);

    const int rc = ex_run(ui, W, H, "QLGL · 14 css3", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
