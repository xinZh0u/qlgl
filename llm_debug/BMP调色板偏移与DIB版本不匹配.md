# BMP 调色板偏移与 DIB 版本不匹配

> **核对 2026-11-15**：✅ 已修 —— `src/draw/qlimage.c:216-217`（`14+hdr_size`、项宽 4）、`:229-230`（COREHEADER 3 字节项）、`:271-272`（V3 BITFIELDS +12）

## 问题

`decode_bmp()` 对所有 `hdr_size >= 40` 的 BMP 都将调色板偏移设为 `14 + 40`，没有跳过大于 40 字节的 V4/V5 DIB 头；同时，8 位 COREHEADER BMP 的调色板项是 3 字节 BGR，但解码器始终按 4 字节步进。

## 影响

上述合法的 8 位 BMP 文件会从头部中间或错误的调色板位置读取颜色，导致图像颜色错乱；边界检查只验证整个输入长度，不能发现读到的是头部/像素数据而非调色板。

## 依据

- `src/draw/qlimage.c` 的 `decode_bmp()` 将 `hdr_size >= 40` 情况的 `palette_off` 固定为 `14 + 40`。
- 8 位索引路径使用 `palette_off + idx * 4`，不区分 COREHEADER 的三字节调色板项。

## 验证状态

由 BMP DIB 头和调色板项布局与解析偏移静态比对确认；未新增特定版本 BMP 样本进行动态测试。
