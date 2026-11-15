# ql_value_get_uint 对 FIXED 类型返回原始定点位，未做换算

> **核对 2026-11-15**：✅ 已修 —— `src/core/qlvalue.c:137` 与 `get_int` 对齐；`:147-153` `get_fixed(UINT)` 64 位饱和

## 现象

`ql_value_get_uint()` 在遇到 `QL_VALUE_FIXED` 值时，直接把定点数的原始 32 位位型
（Q16.16）当作 `uint32_t` 返回，而不是先换算成整数。结果是返回值比预期**放大 65536 倍**。

对比同族的 `ql_value_get_int()`：它对 `QL_VALUE_FIXED` 会调用 `ql_fixed_to_int()`
做四舍五入换算——两个函数对同一类输入的处理不一致。

## 定位

文件：`src/core/qlvalue.c`

```c
int32_t ql_value_get_int(QLValue value, int32_t fallback)
{
    switch (value.type) {
    case QL_VALUE_INT:   return value.v.i;
    case QL_VALUE_UINT:  return (int32_t)value.v.u;
    case QL_VALUE_BOOL:  return value.v.b;
    case QL_VALUE_FIXED: return ql_fixed_to_int(value.v.f);   /* <-- fixed 被换算 */
    case QL_VALUE_COLOR: return (int32_t)value.v.u;
    default:             return fallback;
    }
}

uint32_t ql_value_get_uint(QLValue value, uint32_t fallback)
{
    switch (value.type) {
    case QL_VALUE_UINT:  return value.v.u;
    case QL_VALUE_INT:   return (uint32_t)value.v.i;
    case QL_VALUE_BOOL:  return (uint32_t)value.v.b;
    case QL_VALUE_COLOR: return value.v.u;
    case QL_VALUE_FIXED: return (uint32_t)value.v.f;          /* <-- BUG：没换算 */
    default:             return fallback;
    }
}
```

## 与文档的矛盾

`doc/手册/object/QLObject API手册.md` 第 4 节明确写着：

> | `ql_value_get_int(v, fb)` | int/uint/bool/fixed/color 都接受 |
> | `ql_value_get_uint(v, fb)` | 同上 |

即 `ql_value_get_uint` 也应接受 fixed 并换算，但实现没有。

## 复现

```c
QLValue v = ql_value_fixed(ql_fixed_from_int(100));  /* 0x000186A0 */
uint32_t u = ql_value_get_uint(v, 0);
/* 期望: 100
 * 实际: 6553600  (100 * 65536)
 */

int32_t i = ql_value_get_int(v, 0);   /* 正确返回 100 */
```

负数定点更糟：`ql_value_fixed(-1)` 得到 `0xFFFFFFFF`，
`ql_value_get_uint` 返回 4294967295 而不是 0（或 fallback）。

## 影响

任何把 fixed 值当"可当无符号整数读"的路径都会拿到放大 65536 倍的数：
布局尺寸、进度条比例、颜色通道等都可能算错，且只在"fixed 值被 get_uint 读取"
这一组合下才暴露——常规 `get_int`/`get_fixed` 路径正常，所以不易被发现。

## 建议修复

与 `ql_value_get_int` 对齐：

```c
case QL_VALUE_FIXED: return (uint32_t)ql_fixed_to_int(value.v.f);
```

（或按需求取截断语义 `(uint32_t)(value.v.f >> QL_FIXED_SHIFT)`，但应与
get_int 保持一致并同步更新手册。）
