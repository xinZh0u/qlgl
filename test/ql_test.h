/**
 * @file    ql_test.h
 * @brief   test/object 下所有示例共用的极简断言工具（不依赖任何测试框架）。
 */
#ifndef QL_TEST_H
#define QL_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qlgl/qlgl.h>

static int ql_test_checks;
static int ql_test_failures;

#define QL_TEST_BEGIN(name)                                                  \
    int main(void)                                                           \
    {                                                                        \
        ql_test_checks = 0;                                                  \
        ql_test_failures = 0;                                                \
        printf("== %s ==\n", (name));                                        \
        if (qlgl_init() != QL_OK) {                                          \
            printf("  [FAIL] qlgl_init 失败\n");                             \
            return 1;                                                        \
        }

#define QL_TEST_END()                                                        \
        {                                                                    \
            QLMemStats stats;                                                \
            ql_mem_stats(&stats);                                            \
            QL_CHECK_EQ(stats.bytes_live, 0);                                \
        }                                                                    \
        qlgl_shutdown();                                                     \
        printf("  %d 项检查，%d 项失败\n", ql_test_checks, ql_test_failures); \
        return ql_test_failures == 0 ? 0 : 1;                                \
    }

#define QL_CHECK(cond)                                                       \
    do {                                                                     \
        ql_test_checks++;                                                    \
        if (!(cond)) {                                                       \
            ql_test_failures++;                                              \
            printf("  [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                    \
    } while (0)

#define QL_CHECK_EQ(a, b)                                                    \
    do {                                                                     \
        long long _lhs = (long long)(a);                                     \
        long long _rhs = (long long)(b);                                     \
        ql_test_checks++;                                                    \
        if (_lhs != _rhs) {                                                  \
            ql_test_failures++;                                              \
            printf("  [FAIL] %s:%d  %s == %s（%lld != %lld）\n",             \
                   __FILE__, __LINE__, #a, #b, _lhs, _rhs);                  \
        }                                                                    \
    } while (0)

#define QL_CHECK_STR(a, b)                                                   \
    do {                                                                     \
        const char *_lhs = (a);                                              \
        const char *_rhs = (b);                                              \
        ql_test_checks++;                                                    \
        if (_lhs == NULL || _rhs == NULL || strcmp(_lhs, _rhs) != 0) {       \
            ql_test_failures++;                                              \
            printf("  [FAIL] %s:%d  \"%s\" != \"%s\"\n", __FILE__,           \
                   __LINE__, _lhs ? _lhs : "(null)", _rhs ? _rhs : "(null)");\
        }                                                                    \
    } while (0)

/**
 * GL 上下文在本机本应可用；创建失败通常意味着着色器编译不过，而"静默 SKIP"会让
 * 这类回归一路绿灯通过全部测试。默认把 SKIP 记成失败，只有显式
 * `QLGL_SKIP_GL=1`（例如无 EGL/X11 的机器）才真的放行。
 */
static inline int ql_test_gl_required(void)
{
    const char *v = getenv("QLGL_SKIP_GL");
    return !(v != NULL && v[0] != '\0' && v[0] != '0');
}

#define QL_TEST_GL_SKIP(what)                                                \
    do {                                                                     \
        printf("  [SKIP] %s\n", (what));                                     \
        if (ql_test_gl_required()) {                                         \
            ql_test_checks++;                                                \
            ql_test_failures++;                                              \
            printf("  [FAIL] %s:%d  GL 不可用（确实没有 EGL/GL 请设 "        \
                   "QLGL_SKIP_GL=1）\n", __FILE__, __LINE__);                \
        }                                                                    \
    } while (0)

#endif /* QL_TEST_H */
