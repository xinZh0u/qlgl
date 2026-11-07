/**
 * @file    qlstr.h
 * @brief   自管理字符串。两种容量策略，对应代码规范第 4 条：
 *            QL_STR_COMPACT   每次按实际长度分配（MCU 高频刷新，省内存）
 *            QL_STR_AMORTIZED 容量按 2 倍增长且不缩容（MPU/PC，省分配次数）
 *          策略可按字符串单独切换，不需要改编译开关。
 */
#ifndef QLGL_STR_H
#define QLGL_STR_H

#include "qlgl.h"

typedef enum {
    QL_STR_COMPACT = 0,
    QL_STR_AMORTIZED = 1,
} QLStrPolicy;

typedef struct {
    char     *data;
    uint32_t  len;
    uint32_t  capacity;
    uint8_t   own;        /* 1 = 由 ql_mem 分配，需要释放 */
    uint8_t   policy;     /* QLStrPolicy */
    uint16_t  reserved;
} QLStr;

void        ql_str_init(QLStr *str);
void        ql_str_free(QLStr *str);

/* 设置内容（会复制）；返回 QL_ERR_NOMEM 表示失败且内容保持不变 */
ql_err_t    ql_str_set(QLStr *str, const char *text);
ql_err_t    ql_str_set_n(QLStr *str, const char *text, uint32_t len);
/* 引用外部只读字符串，不复制也不释放（用于字面量） */
ql_err_t    ql_str_set_static(QLStr *str, const char *text);
ql_err_t    ql_str_append(QLStr *str, const char *text);
ql_err_t    ql_str_clear(QLStr *str);

const char *ql_str_data(const QLStr *str);
uint32_t    ql_str_len(const QLStr *str);
int         ql_str_equals(const QLStr *str, const char *text);
void        ql_str_set_policy(QLStr *str, QLStrPolicy policy);
QLStrPolicy ql_str_policy(const QLStr *str);

#define QL_STR_INIT  { NULL, 0, 0, 0, QL_STR_COMPACT, 0 }

#endif /* QLGL_STR_H */
