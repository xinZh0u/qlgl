/**
 * @file    test_text_reject.c
 * @brief   拒绝集合一致性：同一批输入，C 解析器与构建期工具必须**都接受或都拒绝**。
 *
 * 为什么只比"接受/拒绝"而不比错误信息：错误信息是给人看的，措辞会演进；
 * 但"接不接"是语义，两边必须完全一致——否则同一份 UI 在构建期能过、
 * 运行期不能过（或反过来），是最难查的一类问题。
 *
 * 期望值最初由 qlui.py 生成，现在是 qlui_parse.c 自己的接受/拒绝集合（唯一一份）。
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>

#include "../ql_test.h"

typedef struct {
    int         accept;      /* 1 = 应当接受 */
    const char *html;
    const char *css;
    const char *why;         /* 期望拒绝的理由（仅用于打印，便于排查） */
} RejectCase;

static const RejectCase k_cases[] = {
    { 0, "<div id='a'><table></table></div>",
      "", "<input> 第 1 行：不支持的标签 <table>" },
    { 0, "<div><span></div>",
      "", "第 1 行：结束标签 </div> 与 <span> 不匹配" },
    { 0, "<div>",
      "", "标签未闭合：<div>" },
    { 0, "<div></div><div></div>",
      "", "文档必须有且只有一个根元素，当前 2 个" },
    { 0, "<div id='a'></div>",
      "#a { position: sticky }", "t:style[0]: 不支持的 position 'sticky'" },
    { 0, "<div id='a'></div>",
      "#a { width: 1.5em }", "t:style[0]: 不支持的长度 '1.5em'（只支持 px 与 %）" },
    { 0, "<div id='a'></div>",
      "#a { color: #12345 }", "t:style[0]: 非法颜色 '#12345'" },
    { 0, "<div id='a'></div>",
      "@media screen { #a { color: red } }", "t:1: 不支持 at-rule '@media'" },
    { 0, "<div id='a'></div>",
      "@keyframes none { from { opacity: 0 } to { opacity: 1 } }", "t:1: @keyframes 名字 'none' 与 CSS 关键字冲突" },
    { 0, "<div id='a'></div>",
      "#a { transform: rotate(30deg) }", "t:style[0]: transform 只支持 translate()/scale()/scaleX()/scaleY()（收到 'ro" },
    { 0, "<div id='a'></div>",
      "#a { transform: scale(-1) }", "t:style[0]: scale 倍数必须在 0~32.767（收到 'scale(-1)'）" },
    { 0, "<div id='a'></div>",
      "#a { backdrop-filter: blur(0px) }", "t:style[0]: backdrop-filter: blur 半径必须在 1~64px" },
    { 0, "<div id='a'></div>",
      "#a { clip-path: inset(10%) }", "t:style[0]: clip-path 只支持 circle(<百分比>)（0%~100%），收到 'inset(10%)'" },
    { 0, "<div id='a'></div>",
      "#a { z-index: 99999 }", "t:style[0]: z-index 超出 int16 范围" },
    { 0, "<div id='a'></div>",
      "#a { display: grid }", "t:style[0]: 不支持的 display 'grid'" },
    { 0, "<div id='a'></div>",
      "#a { transition: 200ms 300ms }", "transition: 暂不支持 delay（'300ms'）" },
    { 0, "<div id='a'></div>",
      "#a { animation: pulse 1s 2s 3s }", "animation: 时长写了三个（'3s'）" },
    { 0, "<div id='a'></div>",
      "#a { margin: 1px 2px 3px }", "t:style[0]: margin/padding 需要 1/2/4 个值" },
    { 0, "<div id='a'></div>",
      "div + span { color: red }", "不支持相邻/兄弟选择器 '+'" },
    { 0, "<div id='a'></div>",
      "#a:hover { animation: x 1s } @keyframes x { from { opacity: 0 } to { opacity: 1 } }", "<div> 的状态规则里不支持 animation：关键帧动画只按基础样式起播" },
    { 0, "<div id='a'></div>",
      "#a { box-shadow: 0 0 0 }", "t:style[0]: box-shadow 缺少颜色（不支持默认色）" },
    { 1, "<div id='a'></div>",
      "#a { background: linear-gradient(to left, #000, #fff) }", "" },
    { 1, "<div id='a'><p>a<b>b</b></p></div>",
      "", "" },
    { 1, "<div id='a'></div>",
      "#a { font-size: 200px }", "" },
    { 0, "<div id='a'></div>",
      "#a { foo: 1 }", "t:1: 不支持的属性 'foo'" },
    { 1, "<div id='a'></div>",
      "#a { display: inline-flex; justify-content: space-evenly }", "" },
    { 0, "<div id='a'></div>",
      "#a { opacity: 0.5 }", "t:1: 不支持的属性 'opacity'" },
    { 0, "<div id='a'></div>",
      "@keyframes k { 0% { background-color: #000 } 50% { color: #fff } 100% { background-color: #fff } } #a { animation: k 1s }", "@keyframes k: 各帧写的属性不一致（50% 这帧少了别的帧有的属性）" },
    { 0, "<div id='a'></div>",
      "#a { border: 2px dashed #f00 }", "只支持 solid 边框，收到 'dashed'" },
    { 1, "<div id='a'></div>",
      "#a { flex: 1 0 50% }", "" },
    { 0, "<div id='a'></div>",
      "#a { inset: 0 }", "t:1: 不支持的属性 'inset'" },
    { 0, "<div id='a'></div>",
      "#a::before { color: red }", "不支持的简单选择器：'#a::before'" },
    /* 以下这批是从原 test_qlui_errors.py 搬过来的：qlui.py 退化成宿主工具的壳
     * 之后，动画/关键帧的"必须报错"清单就只剩这一份。 */
    { 0, "<div id='a'></div>",
      "@keyframes pulse { 0% { opacity: 1 } 0% { opacity: 0 } }",
      "pulse: 有重复的百分比" },
    { 0, "<div id='a'></div>",
      "@keyframes pulse { 120% { opacity: 1 } }", "关键帧百分比必须在 0%~100%" },
    { 0, "<div id='a'></div>",
      "@keyframes pulse { from { opacity: 0 } to { opacity: 1 } }"
      " #a { animation: pulse 0ms }", "animation-duration 不能为 0" },
    { 0, "<div id='a'></div>",
      "@keyframes pulse { from { opacity: 0 } to { opacity: 1 } }"
      " #a { animation: pulse 1s paused }", "animation-play-state: 暂不支持 paused" },
    { 0, "<div id='a'></div>",
      "@keyframes pulse { from { opacity: 0 } to { opacity: 1 } }"
      " #a { animation: pulse 1s steps(3) }", "暂不支持 steps()" },
    { 0, "<div id='a'></div>",
      "@keyframes pulse { from { opacity: 0 } to { opacity: 1 } }"
      " #a { animation-name: pulse }", "有 animation-name 但没写 animation-duration" },
    { 0, "<div id='a'></div>",
      "@keyframes pulse { from { opacity: 0 } to { opacity: 1 } }"
      " #a { animation: nope 1s }", "animation-name 'nope' 没有对应的 @keyframes" },
    { 0, "<div id='a'></div>",
      "@keyframes pulse { 0% { box-shadow: 0 0 } 100% { box-shadow: 0 0 } }",
      "关键帧里不支持的属性" },
    { 0, "<div id='a'></div>",
      "#a { transform: matrix(1,0,0,1,0,0) }",
      "transform 只支持 translate()/scale()/scaleX()/scaleY()" },
    { 1, "<div id='a'></div>",
      "@keyframes pulse { from { opacity: 0 } to { opacity: 1 } }"
      " #a { animation: pulse 1s 300ms }", "" },
};

#define CASE_COUNT ((int)(sizeof(k_cases) / sizeof(k_cases[0])))

QL_TEST_BEGIN("test_text_reject")

    int mismatch = 0;
    for (int i = 0; i < CASE_COUNT; i++) {
        const RejectCase *tc = &k_cases[i];
        ql_ui_text_set_name("t");
        QLUIDoc *doc = ql_ui_load_text(tc->html, tc->css);
        const int got = (doc != NULL) ? 1 : 0;
        ql_test_checks++;
        if (got != tc->accept) {
            ql_test_failures++;
            mismatch++;
            printf("  [FAIL] #%d 期望%s，实得%s\n    html: %s\n    css : %s\n"
                   "    原因: %s\n", i, tc->accept ? "接受" : "拒绝",
                   got ? "接受" : "拒绝", tc->html, tc->css,
                   got ? "（无）" : ql_ui_text_last_error());
        }
        if (doc != NULL)
            ql_ui_doc_free(doc);
    }
    printf("  %d 个用例，%d 个不一致\n", CASE_COUNT, mismatch);
    ql_ui_text_set_name(NULL);

QL_TEST_END()
