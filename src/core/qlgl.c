/**
 * @file    qlgl.c
 * @brief   库初始化：按生成表注册内置类，并校验 id 与枚举一致。
 */
#include <stdarg.h>
#include <stdio.h>

#include <qlgl/qlgl.h>

/* 由 qlgen 生成 */
extern const QLBuiltinClass ql_builtin_class_table[];
extern const uint16_t ql_builtin_class_count;

void ql_class_reset(void);

static int s_initialized;

const char *qlgl_version(void)
{
    return "0.1.0";
}

void ql_log(int level, const char *fmt, ...)
{
    if (level > QL_CFG_LOG_LEVEL || fmt == NULL)
        return;

    static const char *const tags[] = { "NONE", "ERROR", "WARN", "INFO", "DEBUG" };
    const char *tag = ((unsigned)level < QL_ARRAY_SIZE(tags)) ? tags[level] : "?";

    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[QLGL][%s] ", tag);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

ql_err_t qlgl_init(void)
{
    if (s_initialized)
        return QL_OK;

    if (ql_builtin_class_count == 0)
        return QL_ERR_NOT_INIT;

    ql_mem_reset_stats();
    ql_event_pool_init();
    ql_input_reset();
    ql_widget_reset();
    ql_anim_reset();
    ql_class_reset();

    for (uint16_t i = 0; i < ql_builtin_class_count; i++) {
        const QLBuiltinClass *entry = &ql_builtin_class_table[i];
        QLType type = ql_class_register(entry->klass, entry->parent_type, entry->setup);

        if (type == QL_TYPE_NONE) {
            QL_LOGE("qlgl_init: failed to register %s", entry->name);
            return QL_ERR_INVAL;
        }
        if (type != entry->type) {
            QL_LOGE("qlgl_init: type id mismatch for %s (got %u, expected %u); "
                    "please rerun tools/qlgen",
                    entry->name, (unsigned)type, (unsigned)entry->type);
            return QL_ERR_INVAL;
        }
    }

    s_initialized = 1;
    QL_LOGI("QLGL %s ready, %u builtin classes", qlgl_version(),
            (unsigned)ql_builtin_class_count);
    return QL_OK;
}

void qlgl_shutdown(void)
{
    ql_font_reset_defaults();
    ql_widget_reset();
    ql_event_pool_init();
    ql_input_reset();
    ql_widget_reset();
    ql_anim_reset();
    ql_class_reset();
    ql_mem_reset_stats();
    s_initialized = 0;
}
