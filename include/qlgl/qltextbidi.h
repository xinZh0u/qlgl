/**
 * @file    qltextbidi.h
 * @brief   双向文本（B8）：把逻辑顺序的一行文本排成**视觉顺序**。
 *
 * 两条实现：
 *   1. **fribidi**（首选）：完整的 UAX#9 —— 直连或运行期 `dlopen`；
 *   2. **内建简化 UAX#9**（兜底）：按"单一嵌入层 + 中性字符按两侧方向解析"处理，
 *      覆盖阿拉伯语/希伯来语的常见段落（含里面的数字保持 LTR），不做嵌套嵌入。
 *
 * 用法：画一行之前把这一行的文本重排成视觉顺序，然后**照旧从左往右画**。
 * 为什么画的时候才重排（而不是排布局时）：折行是按逻辑顺序做的，
 * 每行单独重排才是对的；量宽度与顺序无关，所以测量路径不用改。
 */
#ifndef QLGL_TEXT_BIDI_H
#define QLGL_TEXT_BIDI_H

#include "qlgl.h"

/* 一次能重排的最大行宽（字节）。超出的行按逻辑顺序画（正常折行远到不了）。 */
#ifndef QL_BIDI_LINE_MAX
#define QL_BIDI_LINE_MAX  2048
#endif

/*
 * 逻辑顺序 → 视觉顺序。
 *   par_rtl  段落基准方向（`direction: rtl` 时为 1）
 *   out/cap  输出缓冲（可以与 text 相同？不可以 —— 请给独立缓冲）
 * 返回写出的**字节数**；容量不够返回 -1（调用方此时按原顺序画）。
 *
 * 文本里没有强 RTL 字符时**原样拷贝**，所以 LTR 页面的开销只是一次扫描。
 */
int32_t ql_text_bidi_log2vis(const char *text, int32_t len, int par_rtl, char *out,
                             int32_t cap);

/* 这个码位属于强 RTL 吗（阿拉伯/希伯来/叙利亚/它拿/… 的区段判定） */
int ql_text_bidi_is_rtl_cp(uint32_t cp);

/* 这段文本里有强 RTL 字符吗（阿拉伯/希伯来/叙利亚/它拿/…） */
int ql_text_bidi_has_rtl(const char *text, int32_t len);

/* 当前实际生效的后端名："fribidi" / "builtin" */
const char *ql_text_bidi_backend(void);

#endif /* QLGL_TEXT_BIDI_H */
