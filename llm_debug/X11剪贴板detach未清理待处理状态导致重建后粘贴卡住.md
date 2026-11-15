# X11 剪贴板 detach 未清理待处理状态导致重建后粘贴卡住

> **核对 2026-11-15**：✅ 已修 —— `src/port/qlclipboard_x11.h:50-64` detach 清 waiting/in/out 与窗口/Display

## 问题

`ql_x11_clip_detach()` 仅清空 Display 和窗口句柄，没有重置 `s_clip_waiting`、`s_clip_in_ready`、输入长度或上一次剪贴板内容。如果窗口在异步粘贴请求仍等待 SelectionNotify 时被销毁，随后同进程重新创建 X11 后端，旧的 `s_clip_waiting` 会让 `ql_x11_clip_get_text()` 持续返回“仍在等待”，而新 Display 不会收到旧请求的通知。

## 影响

销毁并重建 X11 显示后端期间若有粘贴操作在途，后续粘贴请求可能永久卡住；若旧结果已就绪，则也可能先返回旧剪贴板内容。

## 依据

- `src/port/qlclipboard_x11.h` 的 `ql_x11_clip_detach()` 只清除 `s_clip_dpy` 与 `s_clip_win`。
- `ql_x11_clip_get_text()` 在 `s_clip_waiting` 为真时直接返回，不会为新 Display 发起请求。
- `ql_x11_clip_attach()` 设置新 Display 时也不清理这些异步状态字段。

## 验证状态

静态追踪确认异步状态跨 detach/attach 保留；本次未通过销毁/重建后延迟 SelectionNotify 的时序动态复现。
