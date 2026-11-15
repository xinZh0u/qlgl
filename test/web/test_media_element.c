/**
 * @file    test_media_element.c
 * @brief   `<audio>` 在媒体构建下不产生盒子（与浏览器一致）。
 *
 * 没有媒体能力时它仍是占位盒（300×32），所以这条测试只在
 * `QL_FEAT_UI_MEDIA=1` 的构建里跑。
 */
#include <stdio.h>
#include <string.h>

#include "../ql_test.h"

QL_TEST_BEGIN("test_media_element")

#if QL_FEAT_UI_MEDIA

    QLUIDoc *doc = ql_ui_load_html(
        "<div id=root>"
        "<audio src=\"track.wav\"></audio>"
        "<div id=after>x</div>"
        "</div>",
        "#root { width: 100%; height: 100%; }"
        "#after { height: 20px; background-color: #123456; }");
    QL_CHECK(doc != NULL);
    if (doc != NULL) {
        QLUIRoot *ui = ql_ui_build(doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            ql_ui_layout(ui, 200, 100);

            QLUINodeRT *audio = ql_ui_find_by_tag(ui, QL_UI_TAG_AUDIO, 0);
            QL_CHECK(audio != NULL);
            if (audio != NULL) {
                const QLRect r = ql_ui_node_rect(audio);
                printf("  audio rect = %d,%d %dx%d\n", (int)r.x, (int)r.y,
                       (int)r.w, (int)r.h);
                QL_CHECK_EQ((int)r.w, 0);
                QL_CHECK_EQ((int)r.h, 0);
            }

            /* 紧跟其后的 div 从 y=0 开始：audio 没占任何空间 */
            QLUINodeRT *after = ql_ui_find_by_tag(ui, QL_UI_TAG_DIV, 1);
            QL_CHECK(after != NULL);
            if (after != NULL) {
                const QLRect r = ql_ui_node_rect(after);
                QL_CHECK_EQ((int)r.y, 0);
                QL_CHECK_EQ((int)r.h, 20);
            }
            ql_ui_free(ui);
        }
        ql_ui_doc_free(doc);
    }

#else
    printf("  [SKIP] QL_FEAT_UI_MEDIA=0（<audio> 保持占位盒）\n");
#endif

QL_TEST_END()
