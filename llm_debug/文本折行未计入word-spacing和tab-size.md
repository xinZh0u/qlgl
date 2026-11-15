# 文本折行未计入 `word-spacing` 和 `tab-size`

## 问题

`QLTextWrapOpt` 暴露 `word_spacing` 与 `tab_size`，调用方也将这两个选项传入 `wrap_next()`；但 `wrap_next()` 只计算字形 advance 和 `letter_spacing`，没有将空格后的 `word_spacing` 加入宽度，也没有把制表符按 `tab_size` 个空格折算。

绘制阶段则会通过字形整形/行宽计算应用这些选项，因此折行宽度与实际绘制宽度不一致。

## 影响

启用较大的 `word-spacing` 或非零 `tab-size` 时，文本可能被安排在当前行，但实际绘制宽度超出盒子；也可能被提前折到下一行，造成布局与显示不一致。

## 依据

- `include/qlgl/qldraw2d.h` 的 `QLTextWrapOpt` 明确包含 `word_spacing` 与 `tab_size`。
- `src/draw/qldraw2d.c` 的 `wrap_next()` 仅以 `adv + letter_spacing` 更新累计宽度。
- 同文件绘制路径将相同参数传给 `ql_font_line_width_ls2()` 与 `QLShapeOpt`，后续绘制会应用它们。

## 验证状态

静态对照折行与绘制路径确认两者宽度规则不一致；本次文本测试未专门覆盖这两项参数共同参与折行的情形。
