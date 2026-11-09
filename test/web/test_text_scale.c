/**
 * @file    test_text_scale.c
 * @brief   `transform: scaleX()` 假翻转时**文字不能上下跑**。
 *
 * 这个 bug 的成因值得记下来：
 *
 *   盒子按 (sx, sy) 各轴缩放，但字形只能**整体**缩小（按 min(sx, sy)，
 *   否则会又小又错位）。于是 sy = 1、sx = 0.5 时，盒子高度没变、字却小了一半，
 *   而基线还贴着盒子顶 → 文字在翻转过程中看起来一路往上跑。
 *
 * 修法：非等比缩放时把（缩小后的）文本块在盒子里垂直居中。
 * 这个测试逐帧比"文字墨迹的 y 范围"，等比/非等比都要守住。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ql_test.h"

#define W 300
#define H 160

static const char *k_html = "<div id=root><div class=\"flip\">翻转</div></div>";

static const char *k_css =
    "#root { width: 300px; height: 160px; background-color: #000000; }"
    ".flip { position: absolute; left: 40px; top: 40px; width: 76px; height: 56px;"
    "  background-color: #FFFFFF; color: #000000; font-size: 13px;"
    "  text-align: center; line-height: 56px;"
    "  animation: flip-x 1400ms linear infinite; }"
    "@keyframes flip-x { 0% { transform: scaleX(1); }"
    "  50% { transform: scaleX(0.02); } 100% { transform: scaleX(1); } }";

/* 在盒子范围内找"既不是黑（背景）也不是白（元素底色）"的像素 → 文字的 y 范围 */
static int ink_y_range(QLUIRoot *ui, QLDispDriver *d, int *out_min, int *out_max)
{
  QLCanvas cv;
  if (ql_canvas_from_disp(&cv, d) != QL_OK)
    return 0;
  ql_canvas_clear(&cv, 0x000000);
  ql_ui_render_canvas(ui, &cv);
  int ymin = -1, ymax = -1;
  for (int y = 40; y < 96; y++)
    for (int x = 40; x < 116; x++) {
      const ql_color_t c = ql_disp_mem_pixel(d, x, y);
      if (c != 0x000000 && c != 0xFFFFFF) {
        if (ymin < 0)
          ymin = y;
        ymax = y;
        break;
      }
    }
  *out_min = ymin;
  *out_max = ymax;
  return ymax >= 0;
}

QL_TEST_BEGIN("test_text_scale")

    QLDispDriver *d = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
    QL_CHECK(d != NULL);
    if (d != NULL) {
      QLUIDoc *doc = ql_ui_load_text(k_html, k_css);
      QL_CHECK(doc != NULL);
      if (doc != NULL) {
        QLUIRoot *ui = ql_ui_build(doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
          ql_ui_layout(ui, W, H);

          int base_min = -1, base_max = -1;
          /* 起点：scaleX = 1 */
          (void)ql_ui_anim_tick(ui, 0);
          QL_CHECK(ink_y_range(ui, d, &base_min, &base_max));
          printf("    t=0    文字 y = [%d, %d]\n", base_min, base_max);

          /* 翻转中间几帧：文字的**垂直中心**必须和起点一致 */
          const uint32_t k_ts[] = { 175, 350, 525, 875, 1050, 1225 };
          for (size_t i = 0; i < sizeof(k_ts) / sizeof(k_ts[0]); i++) {
            int ymin = -1, ymax = -1;
            (void)ql_ui_anim_tick(ui, k_ts[i]);
            if (!ink_y_range(ui, d, &ymin, &ymax))
              continue;   /* 极窄的那几帧可能一个像素都不剩，跳过 */
            /*
             * 压到只剩一两个像素宽时，墨迹的竖直范围就只反映"幸存的那几列"
             * （横画没了 → bbox 变矮），那是几何重采样的正常结果，不是上下漂。
             * 只在"还看得出是一行字"的帧上比竖直中心。
             */
            if (ymax - ymin < 6)
              continue;
            printf("    t=%-5u 文字 y = [%d, %d]\n", k_ts[i], ymin, ymax);
            /*
             * 只要求"垂直中心不漂"：等比缩小会让墨迹范围整体变窄，
             * 那是正常的；上下平移才是 bug。
             */
            const int c0 = (base_min + base_max);
            const int c1 = (ymin + ymax);
            QL_CHECK(c1 - c0 <= 2 && c0 - c1 <= 2);
          }
          ql_ui_free(ui);
        }
        ql_ui_doc_free(doc);
      }
      ql_disp_mem_destroy(d);
    }

    /* ---------- 等比缩放仍然按盒子中心缩（不能因为这个修复而变形） ---------- */
    {
      const char *css =
          "#root { width: 300px; height: 160px; background-color: #000000; }"
          ".flip { position: absolute; left: 40px; top: 40px; width: 76px;"
          "  height: 56px; background-color: #FFFFFF; color: #000000;"
          "  font-size: 13px; text-align: center; line-height: 56px;"
          "  animation: zoom 1400ms linear infinite; }"
          "@keyframes zoom { 0% { transform: scale(1); }"
          "  50% { transform: scale(0.6); } 100% { transform: scale(1); } }";
      QLDispDriver *d2 = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
      QL_CHECK(d2 != NULL);
      if (d2 != NULL) {
        QLUIDoc *doc = ql_ui_load_text(k_html, css);
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
          QLUIRoot *ui = ql_ui_build(doc);
          QL_CHECK(ui != NULL);
          if (ui != NULL) {
            ql_ui_layout(ui, W, H);
            int a0 = -1, a1 = -1, b0 = -1, b1 = -1;
            (void)ql_ui_anim_tick(ui, 0);
            QL_CHECK(ink_y_range(ui, d2, &a0, &a1));
            (void)ql_ui_anim_tick(ui, 350);
            QL_CHECK(ink_y_range(ui, d2, &b0, &b1));
            printf("    等比：t=0 [%d,%d] → t=350 [%d,%d]\n", a0, a1, b0, b1);
            QL_CHECK((a0 + a1) - (b0 + b1) <= 2 && (b0 + b1) - (a0 + a1) <= 2);
            ql_ui_free(ui);
          }
          ql_ui_doc_free(doc);
        }
        ql_disp_mem_destroy(d2);
      }
    }

QL_TEST_END()
