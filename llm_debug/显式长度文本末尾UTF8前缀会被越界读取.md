# 显式长度文本末尾 UTF-8 前缀会被越界读取

## 问题

`ql_utf8_decode()` 只接收 NUL 结尾字符串，不接收缓冲区长度；遇到 2/3/4 字节首字节后，会直接读取所需的后续字节。多个支持显式长度的文本路径在剩余长度不足时仍调用它，包括 `ql_text_bidi_log2vis()`、`ql_text_bidi_has_rtl()` 和内建 `ql_text_shape()`。

例如，传入长度为 1、内容仅有一个合法的四字节首字节的缓冲区时，解码器会继续访问该缓冲区之后的三个字节。

## 影响

在调用方提供的长度边界处存在截断 UTF-8 时，文本测量、整形或双向重排可能读取缓冲区之外的数据，导致未定义行为或进程崩溃。

## 依据

- `src/text/qlfont.c` 的 `ql_utf8_decode()` 对每个 continuation byte 直接执行 `s[i]`，没有剩余长度参数。
- `src/text/qltextbidi.c` 的显式长度入口在完成码点解码前只以 `i < len` 检查起始位置。
- `src/text/qltextshape.c` 的 `builtin_shape_raw()` 同样在读取码点前检查当前位置，而未限制解码跨越 `len`。

## 验证状态

由显式长度入口到无界解码器的调用链静态确认；本轮未以 ASan 构造截断 UTF-8 样本复现。
