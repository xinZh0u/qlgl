# 内存跟踪头部尺寸加法溢出且长度字段截断

> **核对 2026-11-15**：✅ 已修 —— `src/core/qlmem.c:13-17` 头部 64 位 size、`:64-66` 头部加法检查、`:138-143` realloc 64 位旧长度

## 问题

启用 `QL_CFG_MEM_TRACK` 时，`ql_mem_alloc()` 直接计算 `sizeof(QLMemHeader) + size`，没有检查 `size_t` 加法溢出。极大的 `size` 可使分配尺寸回绕成很小的值，随后仍返回头部之后的指针，调用方按原请求写入会越界。

同时，`QLMemHeader.size` 只有 32 位，而 API 参数是 `size_t`。超过 `UINT32_MAX` 的申请会截断记录长度；`ql_mem_realloc()` 也以该字段作为旧长度复制数据，可能少复制并错误更新 live-byte 统计。

## 影响

对超大缓冲区请求可能造成分配尺寸低估、越界写入及 realloc 数据截断；释放统计也会与真实分配量不一致。

## 依据

- `src/core/qlmem.c` 的跟踪分支执行 `size_t total = sizeof(QLMemHeader) + size`，没有溢出检查。
- 同一分支将 `size` 强制转换成 `uint32_t` 存入 header。
- `ql_mem_realloc()` 读取该 32 位 `header->size` 作为复制长度。

## 验证状态

静态检查确认边界缺失；未执行超大分配/写入动态测试，以免触发越界行为。
