# 定点整数转换对不可表示输入缺少边界保护

> **核对 2026-11-15**：✅ 已修 —— `src/core/qlvalue.c:10-15`（from_int 64 位 + 饱和）、`:41-46`（to_int 全程 int64）

## 问题位置

- `src/core/qlvalue.c`：`ql_fixed_from_int()`
- `src/core/qlvalue.c`：`ql_fixed_to_int()`

## 问题说明

定点数使用有符号 32 位存储，但转换没有处理不可表示边界：

- `ql_fixed_from_int()` 直接计算 `value * QL_FIXED_ONE`。当 `value` 超出定点格式可表示的整数范围时，有符号乘法溢出，属于未定义行为。
- `ql_fixed_to_int()` 在负数分支计算 `-value`。若 `value == INT32_MIN`，其相反数无法由 `int32_t` 表示，同样触发未定义行为。

## 影响

边界值转换结果不可靠，可能随编译器及优化选项变化。常规小整数转换不受影响。

## 检查状态

由定点数位宽与 C 有符号算术规则静态确认；没有修改项目代码。
