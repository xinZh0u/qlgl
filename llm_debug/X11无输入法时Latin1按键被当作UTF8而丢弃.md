# X11 无输入法时 Latin-1 按键被当作 UTF-8 而丢弃

## 问题

没有 XIC 时，`ql_x11_push_input_scaled()` 使用 `XLookupString()` 获取可打印字符。代码注释已指出该接口返回 Latin-1，但随后无转换地把字节交给 `ql_x11_push_text_utf8()`；后者按 UTF-8 首字节模式识别字符，并在多字节序列不完整时直接停止。

例如 Latin-1 单字节 `0xE9` 会被当作 3 字节 UTF-8 首字节，因后续字节不足而被丢弃。

## 影响

未连接输入法时，非 ASCII Latin-1 字符无法作为文本输入；若字节被其他路径传递，还可能形成无效 UTF-8。X11 CPU 显示与 GL 后端共用该翻译头。

## 依据

- `src/port/qlinput_x11.h` 的无 XIC 分支调用 `XLookupString()`，并注释其返回 Latin-1。
- 同函数随后将结果直接传给 UTF-8 专用的 `ql_x11_push_text_utf8()`，未执行 Latin-1 到 UTF-8 转换。

## 验证状态

静态追踪确认编码契约不匹配；现有 X11 输入测试通过，本次未在无 XIC 环境下按下 Latin-1 扩展字符复现。
