# `ql_str_append()` 扩容后可能从旧缓冲区读取

## 问题位置

- `src/core/qlstr.c`：`ql_str_append()`
- `src/core/qlmem.c`：带内存跟踪时的 `ql_mem_realloc()`

## 复现路径

```c
QLStr s;
ql_str_init(&s);
ql_str_set(&s, "hello");
ql_str_append(&s, ql_str_data(&s));
```

在默认 `QL_CFG_MEM_TRACK=1` 配置下，`ql_str_append()` 先保存源指针 `text`，再调用 `ensure_capacity()`。紧凑策略容量不足时，`ql_mem_realloc()` 会分配新块、复制原内容并释放旧块；随后 `ql_str_append()` 仍用旧的 `text` 执行 `memmove()`，因此读取已释放内存。自追加是正常且合理的调用方式，当前 API 没有禁止源字符串指向自身缓冲区。

`ql_str_set_n()` 也存在相同的别名风险：扩容或缩容导致重新分配后，仍从调用前传入的旧缓冲区指针读取。

## 影响

自追加/以自身子串设置时可能读取已释放内存，导致结果损坏或未定义行为。普通外部字符串输入不受影响。

## 检查状态

问题由读指针生命周期分析确认；没有修改项目代码。
