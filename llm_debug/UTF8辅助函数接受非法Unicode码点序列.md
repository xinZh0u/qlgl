# UTF-8 辅助函数接受非法 Unicode 码点序列

> **核对 2026-11-15**：✅ 已修 —— `src/text/qlfont.c:229-306` 统一 `utf8_seq()`：拒绝 overlong / 代理项 / >U+10FFFF / F5..F7，decode/valid_len/encode 共用

## 问题

`ql_utf8_decode()` 只检查字节形状是否符合 2/3/4 字节模板，没有拒绝过长编码、代理项码点或大于 U+10FFFF 的码点；它还将 `F5..F7` 作为合法 4 字节首字节接受。`ql_utf8_valid_len()` 也只检查 continuation-byte 形状，因此会把这些非法序列计入“合法 UTF-8 前缀”。

反向的 `ql_utf8_encode()` 同样不验证输入是否为 Unicode scalar value：代理项被编码为 3 字节序列，超出 U+10FFFF 的值也会被写成非法 4 字节序列。

## 影响

非法 UTF-8 或非 Unicode 标量值可通过验证并进入文本整形、输入与剪贴板处理，造成跨平台解码结果不一致；调用方也无法依赖 `ql_utf8_valid_len()` 过滤非法文本。

## 依据

- `src/text/qlfont.c` 的 `ql_utf8_decode()` 在拼出 `cp` 后直接返回，没有检查最短编码边界、代理项区间或最大码点。
- 同文件的 `ql_utf8_valid_len()` 只验证 continuation-byte 前缀。
- `ql_utf8_encode()` 对所有大于等于 `0x10000` 的值都走 4 字节分支，未拒绝代理项和超范围值。

## 验证状态

静态检查确认编码/解码边界校验缺失；现有 UTF-8 与文本测试通过，本次未增加非法码点样本验证。
