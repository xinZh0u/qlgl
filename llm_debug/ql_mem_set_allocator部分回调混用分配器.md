# `ql_mem_set_allocator()` 的部分回调回退会混用不兼容的分配器

> **核对 2026-11-15**：✅ 已修 —— `src/core/qlmem.c:43-52` 三个回调必须同时非空，否则整组回落默认族

## 问题位置

- `src/core/qlmem.c`：`ql_mem_set_allocator()`
- `include/qlgl/qlgl.h`：`QLAllocator` API

## 问题说明

设置非空 `QLAllocator` 时，缺失的回调分别回退到系统 `malloc`、`realloc`、`free`，而已提供的回调仍保留。例如只提供自定义 `alloc` 时，分配得到的指针最终会由默认 `free` 释放；只提供自定义 `realloc` 时，默认 `malloc` 得到的指针会交给自定义 `realloc`。

分配、重分配、释放必须来自同一兼容的分配器族。当前实现/API 注释允许部分回调为空，却没有确保剩余回调彼此兼容，因此会对指针使用错误的分配器操作。`QL_CFG_MEM_TRACK=1` 下的 `ql_mem_realloc()` 也会通过 `s_alloc.alloc` 新分配并以 `s_alloc.free` 释放旧指针，仍受此问题影响。

## 影响

使用不兼容的部分自定义分配器时，重分配或释放可能导致堆损坏、崩溃或未定义行为。全套回调均来自同一分配器，或完全使用默认分配器时不受影响。

## 检查状态

由回调选择逻辑与指针所有权关系静态确认；没有修改项目代码。
