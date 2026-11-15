# PNG 的 zlib 头部跨 IDAT 分块时解码失败

> **核对 2026-11-15**：✅ 已修 —— `src/draw/qlimage.c:536-556` `png_idat_head()`、`:507-518` skip 跨块分摊；回归 `test_png`（IDAT 拆 4 段）

## 问题

PNG 允许把 zlib 数据流任意拆分到连续的多个 IDAT 块中，但解码器假设 2 字节 zlib 头完整位于第一个 IDAT 块：

- `decode_png()` 直接从首个 IDAT 数据位置读取 `cmf` 和 `flg`；
- `png_next_idat()` 在首个 IDAT 中固定跳过 2 字节。

若首个 IDAT 只含 zlib 头的第一个字节，第二个字节实际位于下一个 IDAT，头部检查会把前一块 CRC 当作 `flg`；即便头部检查偶然通过，流读取也无法正确跨块跳过头部。

## 影响

部分结构合法、但 zlib 头恰好跨 IDAT 边界的 PNG 图片会被拒绝或解压失败。IDAT 分块可在任意字节边界切分，包含 zlib 头在内。

## 依据

- `decode_png()` 以 `d[first_idat]` 和 `d[first_idat + 1u]` 直接校验 zlib 头，未确认首个 IDAT 数据长度至少为 2。
- `png_next_idat()` 将 `skip=2` 仅应用于首个 IDAT；没有把跨块的头部字节作为一个连续 zlib 流处理。

## 验证状态

静态检查确认实现假设与 PNG IDAT 拼接语义不符；现有 `test_png` / `test_png_fuzz` 均通过，但本次未构造跨头部边界的 PNG 复现样本。
