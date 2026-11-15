# X11 与 Wayland 剪贴板定长截断可能破坏 UTF-8

## 问题

两个平台剪贴板后端都将 UTF-8 字节串直接截到固定缓冲区大小，没有检查截断位置是否为码点边界：

- X11 的 `ql_x11_clip_set_text()`、SelectionNotify 读取路径及输出到调用方的路径；
- Wayland 的 `qlwl_clip_set()`、接收数据累积路径及 `qlwl_clip_get()` 输出路径。

## 影响

较长的剪贴板内容在多字节字符中间截断时，复制/粘贴接口会提供无效 UTF-8，破坏输入框或其他文本消费者中的字符。

## 依据

- `src/port/qlclipboard_x11.h` 的剪贴板 API 以 UTF-8 为输入/输出，但按 `QL_CLIP_MAX` 直接限制字节数。
- `src/port/qlwindow_wayland.h` 的 `qlwl_clip_set()` / `qlwl_clip_get()` 同样按字节容量截断。

## 验证状态

静态追踪确认两种后端均存在未对齐的字节截断；X11 剪贴板测试通过，本次未构造边界落在多字节字符中的内容。
