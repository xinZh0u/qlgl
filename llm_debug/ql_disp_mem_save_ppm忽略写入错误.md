# ql_disp_mem_save_ppm 忽略写入错误

## 问题

`ql_disp_mem_save_ppm()` 对 PPM 头部和像素数据的 `fprintf()`、`fputc()` 返回值均不检查，也忽略 `fclose()` 的失败结果；文件只要成功打开，函数最终就返回 `QL_OK`。

## 影响

磁盘空间不足、设备写入失败或文件系统错误时，可能留下被截断/损坏的 PPM 文件，却向调用方报告保存成功。

## 依据

- `src/draw/qldisp.c` 的 `ql_disp_mem_save_ppm()` 逐像素调用 `fputc()`，未检查其返回值。
- 该函数无条件忽略 `fclose(fp)` 返回值并返回 `QL_OK`。

## 验证状态

静态追踪保存成功路径确认；本轮未模拟文件系统写入失败。
