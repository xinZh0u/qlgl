/**
 * @file    test_transition_backdrop.c
 * @brief   `transition: backdrop-filter` —— 玻璃悬停时折射半径要渐变，不能瞬跳。
 *
 * 参考稿（codepen MYwrMNy）的玻璃在 hover 时把位移 scale 从 1 动画到 1.4；
 * 本库用 `refract(Npx)` 表达同一条位移曲线，所以 N 必须在 transition 里插值。
 *
 * 守三条：
 *   1. 构建期认 `backdrop-filter` 这个可过渡属性（不再报"未知属性"）；
 *   2. 过渡中 `ql_ui_node_style()` 给出的是**中间值**（blur / saturate / refract 一起）；
 *   3. 结束时精确落在目标值，且动画槽释放。
 */
#include "../ql_test.h"

static uint32_t s_now;

static uint32_t fake_now(void)
{
    return s_now;
}

static const QLUIStyle *style_of(QLUIRoot *ui)
{
    QLUINodeRT *node = ql_ui_find_by_tag(ui, QL_UI_TAG_DIV, 0);
    return (node != NULL) ? ql_ui_node_style(node) : NULL;
}

QL_TEST_BEGIN("test_transition_backdrop")

    ql_anim_set_time_source(fake_now);
    s_now = 1000;

    QLUIDoc *doc = ql_ui_load_text(
        "<div id=g></div>",
        "#g { position: absolute; left: 10px; top: 10px; width: 40px; height: 40px;"
        "     background-color: #203040;"
        "     backdrop-filter: blur(2px) saturate(110%) refract(10px);"
        "     transition: backdrop-filter 200ms linear; }"
        "#g:hover { backdrop-filter: blur(6px) saturate(130%) refract(60px); }");
    QL_CHECK(doc != NULL);
    if (doc == NULL) {
        printf("  %d 项检查，%d 项失败\n", ql_test_checks, ql_test_failures);
        return 1;
    }

    QLUIRoot *ui = ql_ui_build(doc);
    QL_CHECK(ui != NULL);
    ql_ui_layout(ui, 100, 100);

    const QLUIStyle *s = style_of(ui);
    QL_CHECK(s != NULL);
    QL_CHECK_EQ((int)s->backdrop_refract, 10);
    QL_CHECK_EQ((int)s->backdrop_blur, 2);
    QL_CHECK_EQ(ql_anim_active(), 0);

    /* 移入：折射从 10px 在 200ms 内长到 60px */
    QL_CHECK_EQ(ql_ui_pointer_move(ui, 20, 20), 1);
    QL_CHECK_EQ(ql_anim_active(), 1);

    QL_CHECK_EQ(ql_ui_anim_tick(ui, 1000), 1);        /* t = 0 */
    QL_CHECK_EQ((int)style_of(ui)->backdrop_refract, 10);

    s_now = 1100;
    QL_CHECK_EQ(ql_ui_anim_tick(ui, 1100), 1);        /* t = 一半 */
    {
        const int mid = (int)style_of(ui)->backdrop_refract;
        QL_CHECK(mid > 25 && mid < 45);
        QL_CHECK((int)style_of(ui)->backdrop_blur > 3);
    }

    s_now = 1200;
    QL_CHECK_EQ(ql_ui_anim_tick(ui, 1200), 0);        /* 结束 */
    QL_CHECK_EQ((int)style_of(ui)->backdrop_refract, 60);
    QL_CHECK_EQ((int)style_of(ui)->backdrop_blur, 6);
    QL_CHECK_EQ(ql_anim_active(), 0);

    /* 移出：反向插值回 10px */
    s_now = 1300;
    QL_CHECK_EQ(ql_ui_pointer_move(ui, 90, 90), 1);
    QL_CHECK_EQ(ql_anim_active(), 1);
    QL_CHECK_EQ(ql_ui_anim_tick(ui, 1300), 1);
    s_now = 1400;
    QL_CHECK_EQ(ql_ui_anim_tick(ui, 1400), 1);
    {
        const int mid = (int)style_of(ui)->backdrop_refract;
        QL_CHECK(mid > 25 && mid < 45);
    }
    s_now = 1500;
    QL_CHECK_EQ(ql_ui_anim_tick(ui, 1500), 0);
    QL_CHECK_EQ((int)style_of(ui)->backdrop_refract, 10);

    ql_anim_set_time_source(NULL);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);

QL_TEST_END()
