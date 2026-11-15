# libjpeg longjmp 后非 volatile 局部解码器状态不确定

## 问题

`jpeg_decode()` 在 `setjmp()` 前创建局部 `cinfo` 与 `jerr`，之后把它们的地址交给 libjpeg；解码过程中 libjpeg 会修改这两个对象。错误回调 `longjmp()` 回来后，代码继续读取 `jerr.msg`，并将 `&cinfo` 传给 `jpeg_destroy_decompress()`。按 C 标准，`setjmp()` 所在函数中、在 `setjmp()` 后被修改且非 `volatile` 的自动存储期对象，其值在 `longjmp()` 后是不确定的。

代码只将像素指针 `px` 声明为 volatile，并未为解码器结构或错误结构提供同类保证。

## 影响

libjpeg 错误路径上的清理可能使用不确定的解码器状态；不同编译器/优化级别下可能出现错误清理、错误信息损坏或不稳定崩溃，坏图输入下的恢复路径因而不可靠。

## 依据

- `src/port/qlimage_jpeg.c` 的 `cinfo`、`jerr` 均为普通局部变量。
- 两者在 `setjmp()` 后被 libjpeg 回调/解码流程修改。
- `setjmp()` 非零返回分支读取 `jerr.msg` 并调用 `jpeg_destroy_decompress(&cinfo)`。

## 验证状态

基于 C 标准的 `setjmp`/`longjmp` 自动变量规则进行静态分析；现有 JPEG 测试未显示运行时失败，但不能消除此未定义/不确定值风险。
