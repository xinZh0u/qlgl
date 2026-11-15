# HarfBuzz 整形过程中缓存字形指针会被后续插入失效

## 问题

`hb_shape_run()` 对每个 HarfBuzz glyph 调用 `ql_font_find_glyph_id()`，并把返回的 `QLGlyph *` 保存到输出数组；整段 glyph 都收集完后，绘制调用方才逐个读取这些指针。

但动态 FreeType 字体的字形缓存会在插入时扩容并释放旧 `glyphs` 数组，且为维持排序会移动已有 `QLGlyph`。因此后续 glyph 的查找/插入可使此前保存的指针悬空或指向别的字形。字体接口文档也明确说明查找返回的指针只保证在下一次查找前有效。

## 影响

使用 HarfBuzz 与动态 FreeType 字体时，整形行中遇到需要新光栅化的 glyph，后续绘制可能读取已释放或错位的字形结构，导致错误字形、崩溃或越界读取。

## 依据

- `src/text/qltextshape.c` 的 `hb_shape_run()` 将逐次 `ql_font_find_glyph_id()` 返回值保存到 `out[n].glyph`。
- `src/text/qlfont_ft.c` 的 `insert_glyph()` 可能调用 `grow_cache()` 释放旧字形数组，并通过 `memmove()` 移动已有字形。
- `include/qlgl/qlfont.h` 注明动态字形指针可能在下一次查找时失效。

## 验证状态

静态追踪确认整形输出保存了不稳定指针；现有文字测试通过，但本次未构造超过缓存容量或乱序插入的新 glyph 序列动态复现。
