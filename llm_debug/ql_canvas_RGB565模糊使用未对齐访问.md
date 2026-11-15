# RGB565 毛玻璃路径可能对未对齐地址执行 16 位访问

## 问题位置

- `src/draw/qlcanvas.c`：`raw_px_get()`、`raw_px_set()`
- `src/draw/qlcanvas.c`：`backdrop_blur_impl()`
- `src/draw/qldisp.c`：`ql_disp_init_fb()`

## 问题说明

RGB565 的原始读写路径把字节地址强转为 `uint16_t *` 后直接解引用。显示驱动初始化只要求 `stride >= width * 2`，没有要求 stride 为偶数或 framebuffer 按 16 位对齐；因此合法的奇数行步长会使后续行地址未对齐。

毛玻璃支持 RGB565，采样/写回会调用这些原始读写函数。在要求对齐访问的架构上，这属于未定义行为，可能触发对齐异常。

## 影响

使用奇数 stride 或未对齐 framebuffer 的 RGB565 画布执行 backdrop blur 时，可能崩溃或产生架构相关结果。常见偶数对齐的屏幕缓冲不受影响。

## 检查状态

由显示驱动接受的 stride 条件与 RGB565 指针解引用路径静态确认；没有修改项目代码。
