/**
 * @file    test_text_parse.c
 * @brief   `ql_ui_load_text()`：C 版 HTML/CSS 解析器 vs 构建期 Python 工具。
 *
 * **唯一判据**：同一份源文本，两边产出的 doc 编码成 blob 之后**逐字节相同**。
 * 这是 M3.5 第 3 步的验收标准——它比"看起来一样"严格得多：样式表的条数、
 * 文本池的顺序、字号的顺序、关键帧的合成顺序，任何一处不同都会暴露。
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ql_test.h"

#include "ui_flex_row.h"
#include "ui_flex_grow.h"
#include "ui_flex_justify.h"
#include "ui_flex_align.h"
#include "ui_flex_wrap.h"
#include "ui_flex_column.h"
#include "ui_flex_posabs.h"
#include "ui_flex_posfix.h"
#include "ui_flex_posrel.h"
#include "ui_flex_zidx.h"
#include "ui_flex_ovf.h"
#include "ui_flex_mixed.h"
#include "ui_flex_frac.h"
#include "ui_flex_paint.h"
#include "ui_flex_dpr.h"
#include "ui_flex_trans.h"
#include "ui_flex_kf.h"
#include "ui_flex_grad.h"
#include "ui_flex_clip.h"
#include "ui_flex_scale.h"
#include "ui_flex_flip.h"
#include "ui_flex_flipdemo.h"
#include "ui_flex_showcase.h"
#include "ui_flex_blur.h"
#include "ui_flex_anim.h"
#include "ui_flex_anim10.h"
#include "ui_flex_bench100.h"

#ifndef QL_TEXT_CASE_DIR
#define QL_TEXT_CASE_DIR "test/web/flex"
#endif

typedef struct {
    const char    *name;
    const QLUIDoc *expect;    /* Python 侧生成的 doc */
} TextCase;

static const TextCase k_cases[] = {
    { "row", &ui_flex_row_doc },
    { "grow", &ui_flex_grow_doc },
    { "justify", &ui_flex_justify_doc },
    { "align", &ui_flex_align_doc },
    { "wrap", &ui_flex_wrap_doc },
    { "column", &ui_flex_column_doc },
    { "posabs", &ui_flex_posabs_doc },
    { "posfix", &ui_flex_posfix_doc },
    { "posrel", &ui_flex_posrel_doc },
    { "zidx", &ui_flex_zidx_doc },
    { "ovf", &ui_flex_ovf_doc },
    { "mixed", &ui_flex_mixed_doc },
    { "frac", &ui_flex_frac_doc },
    { "paint", &ui_flex_paint_doc },
    { "dpr", &ui_flex_dpr_doc },
    { "trans", &ui_flex_trans_doc },
    { "kf", &ui_flex_kf_doc },
    { "grad", &ui_flex_grad_doc },
    { "clip", &ui_flex_clip_doc },
    { "scale", &ui_flex_scale_doc },
    { "flip", &ui_flex_flip_doc },
    { "flipdemo", &ui_flex_flipdemo_doc },
    { "showcase", &ui_flex_showcase_doc },
    { "blur", &ui_flex_blur_doc },
    { "anim", &ui_flex_anim_doc },
    { "anim10", &ui_flex_anim10_doc },
    { "bench100", &ui_flex_bench100_doc },
};

#define CASE_COUNT ((int)(sizeof(k_cases) / sizeof(k_cases[0])))

/* 读整个文件到内存（调用方 free） */
static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    char *buf = (char *)ql_mem_alloc((size_t)n + 1u);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

QL_TEST_BEGIN("test_text_parse")

    for (int i = 0; i < CASE_COUNT; i++) {
        const TextCase *tc = &k_cases[i];
        char hp[512], cp[512];
        snprintf(hp, sizeof(hp), "%s/%s.html", QL_TEXT_CASE_DIR, tc->name);
        snprintf(cp, sizeof(cp), "%s/%s.css", QL_TEXT_CASE_DIR, tc->name);

        char *html = read_file(hp);
        char *css = read_file(cp);
        QL_CHECK(html != NULL);
        QL_CHECK(css != NULL);
        if (html == NULL || css == NULL) {
            ql_mem_free(html);
            ql_mem_free(css);
            continue;
        }

        {
            /* doc 名字必须与构建期工具一致（CMake 传的是 --name flex_<case>），
               否则 blob 头的 name_len 不同，逐字节比对就过不去 */
            char dname[64];
            snprintf(dname, sizeof(dname), "flex_%s", tc->name);
            ql_ui_text_set_name(dname);
        }
        QLUIDoc *got = ql_ui_load_text(html, css);
        ql_mem_free(html);
        ql_mem_free(css);
        QL_CHECK(got != NULL);
        if (got == NULL) {
            printf("  [FAIL] %s: %s\n", tc->name, ql_ui_text_last_error());
            continue;
        }

        /* 计数先比（差异更直观），再比字节 */
        QL_CHECK_EQ(got->node_count, tc->expect->node_count);
        QL_CHECK_EQ(got->style_count, tc->expect->style_count);
        if (got->text_count != tc->expect->text_count)
            printf("  [CASE] %s text %u vs %u\n", tc->name, got->text_count,
                   tc->expect->text_count);
        QL_CHECK_EQ(got->text_count, tc->expect->text_count);
        QL_CHECK_EQ(got->font_size_count, tc->expect->font_size_count);
        QL_CHECK_EQ(got->anim_count, tc->expect->anim_count);
        QL_CHECK_EQ(got->keyframe_count, tc->expect->keyframe_count);

        const size_t need_a = ql_ui_doc_encode(tc->expect, NULL, 0);
        const size_t need_b = ql_ui_doc_encode(got, NULL, 0);
        if (need_b != need_a)
            printf("  [CASE] %s size %zu vs %zu\n", tc->name, need_b, need_a);
        QL_CHECK_EQ(need_b, need_a);
        if (need_a > 0 && need_b == need_a) {
            uint8_t *ba = (uint8_t *)ql_mem_alloc(need_a);
            uint8_t *bb = (uint8_t *)ql_mem_alloc(need_b);
            QL_CHECK(ba != NULL && bb != NULL);
            if (ba != NULL && bb != NULL) {
                QL_CHECK_EQ(ql_ui_doc_encode(tc->expect, ba, need_a), need_a);
                QL_CHECK_EQ(ql_ui_doc_encode(got, bb, need_b), need_b);
                if (memcmp(ba, bb, need_a) != 0) {
                    size_t k = 0;
                    while (k < need_a && ba[k] == bb[k])
                        k++;
                    printf("  [FAIL] %s: blob 第 %zu 字节不同（期望 %02X，实得 %02X）\n",
                           tc->name, k, ba[k], bb[k]);
                    ql_test_failures++;
                    ql_test_checks++;
                }
            }
            ql_mem_free(ba);
            ql_mem_free(bb);
        }

        ql_ui_doc_free(got);
    }

    ql_ui_text_set_name(NULL);

QL_TEST_END()
