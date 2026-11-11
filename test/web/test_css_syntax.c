/**
 * @file    test_css_syntax.c
 * @brief   真实页面里天天出现、以前却被拒的那几种写法（解析层回归）。
 *
 * 这些都不是"新特性"，而是**解析器太窄**：真实 CSS 写 `.5`、写
 * `hsl(120 50% 50%)`、写 `box-shadow: inset ... red`、写 `gap: 1rem`、
 * 写 `linear-gradient(180deg, red 0%, blue 100%)`，一律被"不支持"打回。
 *
 * 每条都验到**具体数值**（样式字段 / 布局尺寸 / 字色），只验"能解析"没意义：
 * 单位折错、颜色转错、位置被悄悄忽略，都只有比对数值才抓得到。
 */
#include <stdlib.h>

#include "../ql_test.h"

#define CW 200
#define CH 120

static QLUIDoc *s_doc;

static QLUIRoot *build(const char *html, const char *css)
{
    s_doc = ql_ui_load_text(html, css);
    if (s_doc == NULL) {
        printf("  编译失败：%s\n", ql_ui_text_last_error());
        return NULL;
    }
    QLUIRoot *ui = ql_ui_build(s_doc);
    if (ui == NULL) {
        ql_ui_doc_free(s_doc);
        return NULL;
    }
    ql_ui_layout(ui, CW, CH);
    return ui;
}

static void drop(QLUIRoot *ui)
{
    ql_ui_free(ui);
    ql_ui_doc_free(s_doc);
    s_doc = NULL;
}

/* 编译必须失败，且报错里含 needle */
static int rejects(const char *css, const char *needle)
{
    QLUIDoc *doc = ql_ui_load_text("<div id=a></div>", css);
    if (doc != NULL) {
        ql_ui_doc_free(doc);
        printf("  期望拒绝但通过了：%s\n", css);
        return 0;
    }
    const char *err = ql_ui_text_last_error();
    if (strstr(err, needle) == NULL) {
        printf("  报错不含 '%s'：%s\n", needle, err);
        return 0;
    }
    return 1;
}

QL_TEST_BEGIN("test_css_syntax")

    /* ---------- 1. 前导小数：`.5` 与 `0.5` 等价 ---------- */
    {
        QLUIRoot *ui = build("<div id=a>x</div>",
                             "#a { opacity: .5; letter-spacing: .5px; "
                             "margin: .5em 0 0 0 }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const QLUIStyle *s = ql_ui_node_style(&ui->nodes[0]);
            QL_CHECK_EQ((int)s->opacity, 128);
            /* .5em 在 16px 字号下 = 8px */
            QL_CHECK_EQ((int)s->margin.top, 8);
            drop(ui);
        }
        /* `1.` 仍然非法（没有小数部分） */
        QL_CHECK(rejects("#a { opacity: 1. }", "opacity"));
    }

    /* ---------- 2. hsl() / rgb() 的空白语法与 / alpha ---------- */
    {
        QLUIRoot *ui = build("<div id=a>x</div>",
                             "#a { color: hsl(120 100% 25%) }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const QLUIStyle *s = ql_ui_node_style(&ui->nodes[0]);
            /* hsl(120,100%,25%) = #008000 */
            QL_CHECK_EQ(s->text_color & 0xFFFFFFu, 0x008000u);
            drop(ui);
        }
        ui = build("<div id=a>x</div>", "#a { color: hsl(120, 100%, 25%) }");
        QL_CHECK(ui != NULL);          /* 逗号写法照样行 */
        if (ui != NULL) {
            QL_CHECK_EQ(ql_ui_node_style(&ui->nodes[0])->text_color & 0xFFFFFFu,
                        0x008000u);
            drop(ui);
        }
        ui = build("<div id=a>x</div>", "#a { color: rgb(255 0 0 / .5) }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ(ql_ui_node_style(&ui->nodes[0])->text_color & 0xFFFFFFu,
                        0xFF0000u);
            drop(ui);
        }
        ui = build("<div id=a>x</div>", "#a { color: rgb(100% 0% 0%) }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ(ql_ui_node_style(&ui->nodes[0])->text_color & 0xFFFFFFu,
                        0xFF0000u);
            drop(ui);
        }
        QL_CHECK(rejects("#a { color: hsl(120 50%) }", "hsl"));
    }

    /* ---------- 2b. 现代颜色函数（宽色域换算到 sRGB） ---------- */
    {
        struct { const char *css; uint32_t rgb; } cases[] = {
            { "#a { color: hwb(0 0% 0%) }", 0xFF0000u },
            { "#a { color: hwb(120 20% 30%) }", 0x33B233u },
            { "#a { color: lab(53.24 80.09 67.20) }", 0xFF0000u },   /* 标准红 */
            { "#a { color: lch(53.24 104.55 40) }", 0xFF0000u },
            { "#a { color: oklab(0.6279 0.2249 0.1258) }", 0xFF0000u },
            { "#a { color: oklch(62.79% 0.2577 29.23) }", 0xFF0000u },
            { "#a { color: color(srgb 1 0 0) }", 0xFF0000u },
            { "#a { color: color(display-p3 1 0 0) }", 0xFF0000u },
            { "#a { color: color(display-p3 0 1 0) }", 0x00FF00u },
            { "#a { color: hwb(0 100% 0%) }", 0xFFFFFFu },            /* 全白 */
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            QLUIRoot *ui = build("<div id=a>x</div>", cases[i].css);
            QL_CHECK(ui != NULL);
            if (ui == NULL)
                continue;
            QL_CHECK_EQ(ql_ui_node_style(&ui->nodes[0])->text_color & 0xFFFFFFu,
                        cases[i].rgb);
            drop(ui);
        }
        QL_CHECK(rejects("#a { color: lab(53 80) }", "参数看不懂"));
        QL_CHECK(rejects("#a { color: color(rec2020 1 0 0) }", "参数看不懂"));
    }

    /* ---------- 2c. 逻辑属性 → 物理属性 ---------- */
    {
        QLUIRoot *ui = build("<div id=a>x</div>",
                             "#a { padding-inline: 4px 8px; margin-block: 2px }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const QLUIStyle *s = ql_ui_node_style(&ui->nodes[0]);
            QL_CHECK_EQ((int)s->padding.left, 4);
            QL_CHECK_EQ((int)s->padding.right, 8);
            QL_CHECK_EQ((int)s->margin.top, 2);
            QL_CHECK_EQ((int)s->margin.bottom, 2);
            drop(ui);
        }
        /* direction: rtl 时 inline-start 落到右边 */
        ui = build("<div id=a>x</div>",
                   "#a { direction: rtl; padding-inline-start: 6px }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ((int)ql_ui_node_style(&ui->nodes[0])->padding.right, 6);
            drop(ui);
        }
        QL_CHECK(rejects("#a { padding-inline: 1px 2px 3px }", "1~2 个值"));
    }

    /* ---------- 3. box-shadow / text-shadow 的命名色 ---------- */
    {
        /* 以前"第一段字母"被当成颜色：`inset 0 0 2px red` 认成 inset、
           `0 1px 2px black` 认成 px，于是命名色一律报"无法识别" */
        QLUIRoot *ui = build("<div id=a>x</div>",
                             "#a { box-shadow: inset 0 0 2px red }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const QLUIStyle *s = ql_ui_node_style(&ui->nodes[0]);
            QL_CHECK_EQ((int)s->shadow[0].inset, 1);
            QL_CHECK_EQ(s->shadow[0].color & 0xFFFFFFu, 0xFF0000u);
            drop(ui);
        }
        ui = build("<div id=a>x</div>", "#a { box-shadow: 0 0 2px black }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ(ql_ui_node_style(&ui->nodes[0])->shadow[0].color & 0xFFFFFFu,
                        0x000000u);
            drop(ui);
        }
        ui = build("<div id=a>x</div>", "#a { text-shadow: 0 1px 2px black }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ(ql_ui_node_style(&ui->nodes[0])->text_shadow.color & 0xFFFFFFu,
                        0x000000u);
            drop(ui);
        }
        /* 颜色缺失仍然报错（不猜 currentColor） */
        QL_CHECK(rejects("#a { box-shadow: 1px 1px 2px }", "缺少颜色"));
    }

    /* ---------- 4. 渐变：省略方向、两端色标位置 ---------- */
    {
        QLUIRoot *ui = build("<div id=a>x</div>",
                             "#a { background: linear-gradient(red, blue) }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const QLUIStyle *s = ql_ui_node_style(&ui->nodes[0]);
            /* 省略方向 = to bottom（CSS 默认） */
            QL_CHECK_EQ((int)s->bg_grad_dir, 1);
            QL_CHECK_EQ(s->bg_color & 0xFFFFFFu, 0xFF0000u);
            QL_CHECK_EQ(s->bg_color2 & 0xFFFFFFu, 0x0000FFu);
            drop(ui);
        }
        ui = build("<div id=a>x</div>",
                   "#a { background: linear-gradient(180deg, red 0%, blue 100%) }");
        QL_CHECK(ui != NULL);          /* 两端位置与不写等价 */
        if (ui != NULL) {
            QL_CHECK_EQ(ql_ui_node_style(&ui->nodes[0])->bg_color & 0xFFFFFFu,
                        0xFF0000u);
            drop(ui);
        }
        /* 中间位置 / 多色标会改变画面：明确拒绝，不静默近似 */
        QL_CHECK(rejects("#a { background: linear-gradient(180deg, red 20%, blue 80%) }",
                         "0% 与 100%"));
        QL_CHECK(rejects("#a { background: linear-gradient(red, green, blue) }",
                         "两个色标"));
        QL_CHECK(rejects("#a { background: linear-gradient(45deg, red, blue) }",
                         "不支持的渐变方向"));
        /* 色标可以各带 alpha：`linear-gradient(#fff, rgba(255,255,255,0))` 是"上实下透" */
        ui = build("<div id=a>x</div>",
                   "#a { background: linear-gradient(#FF0000, rgba(0,0,255,0)) }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const QLUIStyle *s = ql_ui_node_style(&ui->nodes[0]);
            QL_CHECK_EQ(s->bg_alpha, 255);       /* 起色不透明 */
            QL_CHECK_EQ(s->bg_alpha2, 0);        /* 收色全透 */
            QL_CHECK_EQ(s->bg_color2 & 0xFFFFFFu, 0x0000FFu);
            drop(ui);
        }
        /* 两端都不带 alpha 时两栏都是 255（旧行为） */
        ui = build("<div id=a>x</div>", "#a { background: linear-gradient(red, blue) }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const QLUIStyle *s = ql_ui_node_style(&ui->nodes[0]);
            QL_CHECK_EQ(s->bg_alpha, 255);
            QL_CHECK_EQ(s->bg_alpha2, 255);
            drop(ui);
        }
    }

    /* ---------- 4b. backdrop-filter：blur + saturate，顺序随意 ---------- */
    {
        QLUIRoot *ui = build("<div id=a>x</div>",
                             "#a { backdrop-filter: blur(8px) saturate(150%) }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const QLUIStyle *s = ql_ui_node_style(&ui->nodes[0]);
            QL_CHECK_EQ((int)s->backdrop_blur, 8);
            QL_CHECK_EQ((int)s->backdrop_sat, 150);
            drop(ui);
        }
        /* 换个顺序结果一样 */
        ui = build("<div id=a>x</div>",
                   "#a { backdrop-filter: saturate(150%) blur(8px) }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const QLUIStyle *s = ql_ui_node_style(&ui->nodes[0]);
            QL_CHECK_EQ((int)s->backdrop_blur, 8);
            QL_CHECK_EQ((int)s->backdrop_sat, 150);
            drop(ui);
        }
        /* 不写 saturate 就是"不变"，与 100% 同义 */
        ui = build("<div id=a>x</div>", "#a { backdrop-filter: blur(6px) }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const QLUIStyle *s = ql_ui_node_style(&ui->nodes[0]);
            QL_CHECK_EQ((int)s->backdrop_sat, 0);
            drop(ui);
        }
        /* 100% 归一化成 0（"不变"只有一种表示，绘制侧不用分情况） */
        ui = build("<div id=a>x</div>", "#a { backdrop-filter: saturate(100%) blur(6px) }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ((int)ql_ui_node_style(&ui->nodes[0])->backdrop_sat, 0);
            drop(ui);
        }
    }

    /* ---------- 5. gap 的 em / rem ---------- */
    {
        QLUIRoot *ui = build("<div id=r><div id=a>x</div><div id=b>y</div></div>",
                             "#r { display: flex; gap: 1rem } #r > div { width: 20px }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            /* 根字号默认 16px → 1rem = 16px */
            const QLUINodeRT *a = &ui->nodes[1];
            const QLUINodeRT *b = &ui->nodes[2];
            QL_CHECK_EQ((int)(b->border_box.x - (a->border_box.x + a->border_box.w)), 16);
            drop(ui);
        }
        ui = build("<div id=r><div id=a>x</div><div id=b>y</div></div>",
                   "#r { display: flex; gap: 0.5em; font-size: 20px }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const QLUINodeRT *a = &ui->nodes[1];
            const QLUINodeRT *b = &ui->nodes[2];
            /* 0.5em 相对容器自己的 20px 字号 = 10px */
            QL_CHECK_EQ((int)(b->border_box.x - (a->border_box.x + a->border_box.w)), 10);
            drop(ui);
        }
        QL_CHECK(rejects("#a { gap: 5% }", "不支持百分比"));
    }

    /* ---------- 5b. margin / padding 的 % / vw / vh / vmin / vmax ---------- */
    {
        /*
         * 基准是**包含块宽度**（竖排也一样，这是 CSS 的规定）：
         * 父容器内容宽 200 → `padding: 10%` 四边各 20px。
         */
        QLUIRoot *ui = build("<div id=r><div id=a>x</div></div>",
                             "#r { width: 200px; height: 100px } #a { padding: 10% }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const QLUINodeRT *a = &ui->nodes[1];
            QL_CHECK_EQ((int)a->padding_px.left, 20);
            QL_CHECK_EQ((int)a->padding_px.top, 20);
            /* 内容宽 = 200 - 2×20（border/padding 之外还有文本自身高度） */
            QL_CHECK_EQ((int)a->content_box.w, 160);
            drop(ui);
        }
        ui = build("<div id=r><div id=a>x</div></div>",
                   "#r { width: 200px; height: 100px } #a { margin: 10% }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const QLUINodeRT *a = &ui->nodes[1];
            QL_CHECK_EQ((int)a->border_box.x, 20);
            QL_CHECK_EQ((int)a->border_box.y, 20);
            QL_CHECK_EQ((int)a->border_box.w, 160);
            drop(ui);
        }
        /* 视口单位按**视口**算：测试视口宽 200 → 5vw = 10px */
        ui = build("<div id=r><div id=a>x</div></div>",
                   "#r { width: 200px; height: 100px } #a { padding: 5vw }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ((int)ui->nodes[1].padding_px.left, 10);
            drop(ui);
        }
        /* 两个值：上下 / 左右 */
        ui = build("<div id=r><div id=a>x</div></div>",
                   "#r { width: 200px; height: 100px } #a { padding: 10% 5% }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ((int)ui->nodes[1].padding_px.top, 20);
            QL_CHECK_EQ((int)ui->nodes[1].padding_px.left, 10);
            drop(ui);
        }
        /* auto 仍然按剩余空间分配（不能被当 0） */
        ui = build("<div id=r><div id=a>x</div></div>",
                   "#r { width: 200px; height: 100px } #a { width: 100px; margin: 0 auto }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ((int)ui->nodes[1].border_box.x, 50);
            drop(ui);
        }
        /* calc 里带 % 仍明确报错（边距不接受运行期表达式） */
        QL_CHECK(rejects("#a { margin: calc(10% - 2px) }", "构建期"));
    }

    /* ---------- 6. line-height 的 em / % ---------- */
    {
        int ref = -1;
        const char *variants[] = {
            "#a { line-height: 1.5 }",
            "#a { line-height: 1.5em }",
            "#a { line-height: 150% }",
            "#a { line-height: 24px }",       /* 16px × 1.5 = 24px */
        };
        for (size_t i = 0; i < sizeof(variants) / sizeof(variants[0]); i++) {
            QLUIRoot *ui = build("<div id=a>x</div>", variants[i]);
            QL_CHECK(ui != NULL);
            if (ui == NULL)
                continue;
            const int lh = (int)ql_ui_node_style(&ui->nodes[0])->line_h;
            if (ref < 0)
                ref = lh;
            QL_CHECK_EQ(lh, ref);
            QL_CHECK_EQ(lh, 24);
            drop(ui);
        }
    }

    /* ---------- 7. 过渡：delay / steps() / cubic-bezier() ---------- */
    {
        QLUIRoot *ui = build("<div id=a>x</div>",
                             "#a { transition: opacity .3s ease .2s }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const QLUIStyle *s = ql_ui_node_style(&ui->nodes[0]);
            QL_CHECK_EQ((int)s->trans_ms, 300);
            QL_CHECK_EQ((int)s->trans_delay_ms, 200);
            drop(ui);
        }
        ui = build("<div id=a>x</div>",
                   "#a { transition: opacity .3s cubic-bezier(.34, 1.56, .64, 1) }");
        QL_CHECK(ui != NULL);          /* 逗号在括号里，不能被当成分组 */
        if (ui != NULL) {
            const QLUIStyle *s = ql_ui_node_style(&ui->nodes[0]);
            QL_CHECK_EQ((int)s->trans_bez_on, 1);
            QL_CHECK_EQ((int)s->trans_bez[0], 340);
            QL_CHECK_EQ((int)s->trans_bez[1], 1560);   /* y 可以越界（回弹） */
            drop(ui);
        }
        ui = build("<div id=a>x</div>", "#a { transition: opacity .3s steps(4) }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ((int)ql_ui_node_style(&ui->nodes[0])->trans_steps, 4);
            drop(ui);
        }
        /* 长写 transition-delay 单独用 */
        ui = build("<div id=a>x</div>",
                   "#a { transition-duration: .2s; transition-delay: .1s }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ((int)ql_ui_node_style(&ui->nodes[0])->trans_delay_ms, 100);
            drop(ui);
        }
        /* 贝塞尔的 x 必须在 [0,1] */
        QL_CHECK(rejects("#a { transition: opacity .3s cubic-bezier(1.2,0,0,1) }",
                         "必须在 0~1"));
    }

    /* ---------- 8. 动画：steps() / cubic-bezier() ---------- */
    {
        const char *kf = "@keyframes pulse { from { opacity: 0 } to { opacity: 1 } } ";
        QLUIRoot *ui = build("<div id=a>x</div>",
                             "@keyframes pulse { from { opacity: 0 } to { opacity: 1 } }"
                             " #a { animation: pulse 1s steps(3) }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK(s_doc->anim_count >= 1);
            if (s_doc->anim_count >= 1)
                QL_CHECK_EQ((int)s_doc->anims[0].steps, 3);
            drop(ui);
        }
        ui = build("<div id=a>x</div>",
                   "@keyframes pulse { from { opacity: 0 } to { opacity: 1 } }"
                   " #a { animation: pulse 1s cubic-bezier(.2, .3, .4, 1) }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ((int)s_doc->anims[0].bez_on, 1);
            QL_CHECK_EQ((int)s_doc->anims[0].bez[2], 400);
            drop(ui);
        }
        /* 帧级 cubic-bezier */
        ui = build("<div id=a>x</div>",
                   "@keyframes pulse { from { opacity: 0; "
                   "animation-timing-function: cubic-bezier(.1,.2,.3,.4) } "
                   "to { opacity: 1 } } #a { animation: pulse 1s both }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ((int)s_doc->keyframes[0].bez_on, 1);
            drop(ui);
        }
        QL_CHECK(rejects("@keyframes p { from { opacity: 0 } to { opacity: 1 } } "
                         "#a { animation: p 1s steps(0) }",
                         "1~255"));
        QL_CHECK(strlen(kf) > 0);
        (void)kf;
    }

QL_TEST_END()
