# FriBidi 字符分类函数被按有返回值函数调用

## 问题

FriBidi 的公开 API `fribidi_get_bidi_types()` 返回 `void`，但项目将其声明为返回 `int` 的函数指针，并以返回值是否非零决定是否继续双向处理。通过该不兼容函数指针调用并读取未定义的返回值，可能使有效的 RTL 行被错误地跳过，也可能因平台/编译器差异产生不稳定结果。

直连构建同样将真实函数地址强制转换为该错误签名；运行期加载路径也使用相同签名。

## 影响

依赖 FriBidi 的双向文本在不同 ABI、优化级别或库实现下可能不重排，或出现不稳定行为。内建双向路径不能保证与完整 UAX#9 等价。

## 依据

- FriBidi 公开头文件将 `fribidi_get_bidi_types(const FriBidiChar *, FriBidiStrIndex, FriBidiCharType *)` 声明为 `void`。
- `src/text/qltextbidi.c` 将 `ql_fribidi_types_fn` typedef 为返回 `int`，并在初始化和实际文本处理时检查其返回值。

## 验证状态

已对照 FriBidi 上游公开头文件确认签名不匹配。现有测试在当前环境显示 `fribidi` 后端且通过，但不能验证其他 ABI/优化构建下未定义返回值的行为。
