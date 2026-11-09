/**
 * @file    test_transition_shorthand.c
 * @brief   `transition` 简写的逗号写法与 `transition-property` 的属性表。
 *
 * 坑：旧实现把逗号**换成空格**再一律按一个整体解析，于是
 *   transition: background-color 200ms ease-out, color 200ms ease-out
 * 被读成"一个 200ms + 另一个 200ms" → 报「暂不支持 delay（'200ms'）」。
 * 那句报错对 `200ms 50ms` 是对的，对逗号写法完全是误导。
 *
 * 现在按逗号分组，每组各写属性/时长/缓动：
 *   - 属性取并集（`QLUIStyle.trans_props` 本来就是位图）；
 *   - 时长与缓动**只有一组**（`trans_ms` / `trans_ease`），所以各组必须一致，
 *     不一致就明确报错，不静默取其中一个。
 */
#include "../ql_test.h"

typedef struct {
    int      ms;
    int      ease;
    unsigned props;
} Trans;

/* 只要一条规则、一个节点：返回该节点的过渡参数 */
static int trans_of(const char *css, Trans *out)
{
    const char *H = "<div id=r><div id=a></div></div>";
    QLUIDoc *doc = ql_ui_load_text(H, css);
    if (doc == NULL) {
        printf("  编译失败：%s\n", ql_ui_text_last_error());
        return 0;
    }
    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        return 0;
    }
    const QLUIStyle *st = ql_ui_node_style(&ui->nodes[1]);
    out->ms = (int)st->trans_ms;
    out->ease = (int)st->trans_ease;
    out->props = st->trans_props;
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    return 1;
}

/* 编译期必须失败，且报错里含 needle */
static int rejects(const char *css, const char *needle)
{
    const char *H = "<div id=r><div id=a></div></div>";
    QLUIDoc *doc = ql_ui_load_text(H, css);
    if (doc != NULL) {
        ql_ui_doc_free(doc);
        printf("  本该报错却编译通过了：%s\n", css);
        return 0;
    }
    const char *msg = ql_ui_text_last_error();
    if (msg == NULL || strstr(msg, needle) == NULL) {
        printf("  报错文案不含 '%s'：%s\n", needle, msg ? msg : "(null)");
        return 0;
    }
    return 1;
}

QL_TEST_BEGIN("test_transition_shorthand")

    const char *R = "#r { width: 100px }";

    /* ---------- 1. 单组、不写属性 = all ---------- */
    Trans t;
    char css[256];
    snprintf(css, sizeof(css), "%s #a { transition: 180ms ease-out }", R);
    QL_CHECK(trans_of(css, &t));
    QL_CHECK_EQ(t.ms, 180);
    QL_CHECK_EQ(t.ease, QL_EASE_OUT);
    QL_CHECK_EQ((int)t.props, (int)QL_UI_TRANS_ALL);

    /* ---------- 2. 逗号写多个属性：以前这里报「delay」 ---------- */
    snprintf(css, sizeof(css), "%s #a { transition: background-color 200ms ease-out, "
                               "color 200ms ease-out }", R);
    QL_CHECK(trans_of(css, &t));
    QL_CHECK_EQ(t.ms, 200);
    QL_CHECK_EQ((int)t.props, (int)(QL_UI_TRANS_BG | QL_UI_TRANS_TEXT));

    /* 顺序反过来一样 */
    snprintf(css, sizeof(css), "%s #a { transition: color 200ms, "
                               "background-color 200ms }", R);
    QL_CHECK(trans_of(css, &t));
    QL_CHECK_EQ((int)t.props, (int)(QL_UI_TRANS_BG | QL_UI_TRANS_TEXT));
    QL_CHECK_EQ(t.ease, QL_EASE_CSS);   /* 不写缓动 = ease */

    /* ---------- 3. 有一组不写属性 → 该组是 all，并上其它组 ---------- */
    snprintf(css, sizeof(css), "%s #a { transition: 200ms, transform 200ms }", R);
    QL_CHECK(trans_of(css, &t));
    QL_CHECK_EQ((int)t.props, (int)QL_UI_TRANS_ALL);

    /* ---------- 4. 长写也可以列多个属性 ---------- */
    snprintf(css, sizeof(css), "%s #a { transition-duration: 100ms; "
                               "transition-property: background-color, color }", R);
    QL_CHECK(trans_of(css, &t));
    QL_CHECK_EQ(t.ms, 100);
    QL_CHECK_EQ((int)t.props, (int)(QL_UI_TRANS_BG | QL_UI_TRANS_TEXT));

    snprintf(css, sizeof(css), "%s #a { transition-duration: 100ms; "
                               "transition-property: all }", R);
    QL_CHECK(trans_of(css, &t));
    QL_CHECK_EQ((int)t.props, (int)QL_UI_TRANS_ALL);

    /* ---------- 5. 两个时长才有资格叫 delay ---------- */
    snprintf(css, sizeof(css), "%s #a { transition: background-color 200ms 50ms }", R);
    QL_CHECK(rejects(css, "暂不支持 delay（'50ms'）"));

    /* ---------- 6. 各段时长/缓动不一致：明确报错，不偷偷取一个 ---------- */
    snprintf(css, sizeof(css), "%s #a { transition: background-color 200ms, "
                               "opacity 120ms }", R);
    QL_CHECK(rejects(css, "各段的时长必须相同"));

    snprintf(css, sizeof(css), "%s #a { transition: background-color 200ms linear, "
                               "opacity 200ms ease-in }", R);
    QL_CHECK(rejects(css, "各段的缓动必须相同"));

    /* ---------- 7. 其它仍然要报错的情形 ---------- */
    snprintf(css, sizeof(css), "%s #a { transition: box-shadow 200ms }", R);
    QL_CHECK(rejects(css, "无法识别的部分 'box-shadow'"));

    snprintf(css, sizeof(css), "%s #a { transition: background-color 200ms, }", R);
    QL_CHECK(rejects(css, "是空的"));

    snprintf(css, sizeof(css), "%s #a { transition-duration: 100ms; "
                               "transition-property: box-shadow }", R);
    QL_CHECK(rejects(css, "暂不支持的 transition-property 'box-shadow'"));

QL_TEST_END()
