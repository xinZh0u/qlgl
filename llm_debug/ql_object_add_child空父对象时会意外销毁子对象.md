# ql_object_add_child 空父对象时会意外销毁子对象

## 问题

`ql_object_add_child(parent, child)` 仅检查 `child` 是否为空。若 `parent == NULL`，内部 `ql_object_set_parent(child, NULL)` 会成功（表示解绑），随后 `ql_object_add_child()` 仍无条件释放调用方持有的子对象引用。

对一个无父对象、引用计数为 1 的有效对象调用此 API，会立即销毁对象并返回 `QL_OK`，而调用方通常会将成功误解为挂接成功。

## 影响

空父对象参数可能静默销毁调用方对象；若对象原本已挂在其他父对象下，也会先解绑再销毁。

## 依据

- `src/core/qlobject.c` 的 `ql_object_add_child()` 仅拒绝空 `child`。
- 函数在 `ql_object_set_parent(child, parent)` 返回成功后无条件执行 `ql_object_unref(child)`。
- `ql_object_set_parent(child, NULL)` 的语义是合法解绑，因此无法阻止上述流程。

## 验证状态

由引用计数与控制流静态追踪确认；尚未添加或运行专门的空父参数测试。
