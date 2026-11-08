/**
 * @file    qluitext.h
 * @brief   运行期把 HTML + CSS 文本编译成 `QLUIDoc`（M3.5 第 3 步）。
 *
 * **只有一份实现**：构建期工具 `tools/qltext/qltext.c` 调的就是这里。
 * 同一份源文本，构建期落盘的静态表与运行期算出的 doc 编码成 blob 之后
 * **逐字节相同**（`test_golden` 用提交在 `test/golden` 里的基准守着）。
 * 所以这里不做任何"更聪明"的事——规则一旦变化，基准会立刻红。
 *
 * 三条设计约束：
 *   1. **只在加载时算一次**：没有运行期 CSS 匹配，也没有每帧解析。算完就是静态表；
 *   2. **外来数据必须有界**：节点/样式/文本/规则/嵌套深度全部有 `QL_CFG_UI_MAX_*` 上限；
 *   3. **明确失败**：任何不支持的东西都返回 NULL + 带行列号的原因，绝不"跳过坏数据继续"。
 *
 * 可摘：本文件是独立编译单元，core 里没有任何静态引用指向它。
 * MCU 侧不把 `qlui_parse.c` 加进构建就是零字节成本（界面仍然走构建期生成的静态表）。
 */
#ifndef QLGL_UITEXT_H
#define QLGL_UITEXT_H

#include "qlgl.h"

/*
 * 把 HTML + CSS 编译成 doc。
 *
 * 成功返回 doc（由 arena 持有，用 `ql_ui_doc_free` 释放）；
 * 失败返回 NULL，原因用 `ql_ui_text_last_error()` 取（含 `文件:行` 形式的位置）。
 *
 * html / css 都是 **NUL 结尾的 UTF-8 文本**，可以为空字符串（但 HTML 必须有根元素）。
 * 传入的缓冲不会被保留引用，返回后调用方可以立刻释放。
 */
QLUIDoc *ql_ui_load_text(const char *html, const char *css);

/* 上一次 `ql_ui_load_text` 失败的原因（成功时不改）。永不返回 NULL */
const char *ql_ui_text_last_error(void);

/*
 * 两个入口共用的"名字"：写进 `QLUIDoc.name`。
 * 默认是 `"text"`；设过之后只影响新编译出来的 doc（用于日志/热重载匹配）。
 */
void ql_ui_text_set_name(const char *name);

#endif /* QLGL_UITEXT_H */
