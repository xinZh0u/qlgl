# `QLStr` 长度运算溢出可导致越界访问或分配循环

## 问题位置

- `src/core/qlstr.c`：`ql_str_set_n()` 计算 `len + 1`
- `src/core/qlstr.c`：`ql_str_append()` 计算 `old_len + extra + 1`
- `src/core/qlstr.c`：`ensure_capacity()` 中 amortized 容量扩张循环

## 问题说明

`QLStr.len` 和容量均为 `uint32_t`，但请求分配的大小在加一及累加前没有做溢出检查。

- `ql_str_set_n(str, text, UINT32_MAX)` 中 `len + 1` 回绕为 0。容量检查可能以零长度通过，之后仍执行 `str->data[len] = '\0'`，造成越界写。
- `ql_str_append()` 中 `old_len + extra + 1` 可回绕，导致容量不足时仍继续拷贝并写结尾 NUL。
- amortized 模式扩容时 `capacity *= 2` 可回绕为 0；当 `need` 仍大于容量，`while (capacity < need)` 可能永不结束。

## 影响

传入极大长度或字符串累计长度接近 `UINT32_MAX` 时，可能发生越界读写、内存破坏或进程卡死。正常小字符串不受影响。

## 检查状态

此问题由长度类型与运算路径的静态检查发现；尚未执行需要超大内存的动态复现。没有修改项目代码。
