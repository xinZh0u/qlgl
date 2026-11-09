/**
 * @file    qlgl.h
 * @brief   QLGL 总入口头文件。使用者只需 #include <qlgl/qlgl.h>。
 */
#ifndef QLGL_H
#define QLGL_H

#include <stddef.h>
#include <stdint.h>

#include "qlconfig.h"

#define QLGL_VERSION_MAJOR  0
#define QLGL_VERSION_MINOR  1
#define QLGL_VERSION_PATCH  0

/* ----- 错误码：统一负值，便于 if (err < 0) 判断 ----- */
typedef enum {
    QL_OK           = 0,
    QL_ERR_NOMEM    = -1,   /* 内存不足 */
    QL_ERR_INVAL    = -2,   /* 参数非法 / id 越界 */
    QL_ERR_NOT_INIT = -3,   /* 未初始化 / 父类未注册 */
    QL_ERR_OVERFLOW = -4,   /* 私有区或继承深度超限 */
    QL_ERR_EXISTS   = -5,   /* 重复注册 */
    QL_ERR_VER      = -6,   /* 版本不兼容 */
    QL_ERR_FULL     = -7,   /* 池已满 */
    QL_ERR_UNSUPPORTED = -8, /* 这个构建没启用该能力（格式/特性被裁掉） */
} ql_err_t;

/* ----- 公共宏 ----- */
#define QL_ARRAY_SIZE(a)    (sizeof(a) / sizeof((a)[0]))
#define QL_UNUSED(x)        ((void)(x))

/* 32 位固定的"无"值，禁止使用 -1/0 之类的魔法数字 */
#define QL_TYPE_NONE        0xFFFFFFFFu
#define QL_CONN_ID_NONE     0u
#define QL_CONN16_NONE      0xFFFFu
#define QL_INDEX_NONE       0xFFFFu

/* 对象的 32 位 id 与 16 位池下标刻意分开，前者对外、后者对内 */
typedef uint32_t ql_object_id_t;

/* 类型 id：32 位固定，禁止改成 unsigned long（32/64 位不等长） */
typedef uint32_t QLType;

/* ----- 生命周期 ----- */
ql_err_t    qlgl_init(void);
void        qlgl_shutdown(void);
const char *qlgl_version(void);

/* ----- 日志 ----- */
#define QL_LOG_LEVEL_NONE   0
#define QL_LOG_LEVEL_ERROR  1
#define QL_LOG_LEVEL_WARN   2
#define QL_LOG_LEVEL_INFO   3
#define QL_LOG_LEVEL_DEBUG  4

void ql_log(int level, const char *fmt, ...);

#define QL_LOGE(...) ql_log(QL_LOG_LEVEL_ERROR, __VA_ARGS__)
#define QL_LOGW(...) ql_log(QL_LOG_LEVEL_WARN,  __VA_ARGS__)
#define QL_LOGI(...) ql_log(QL_LOG_LEVEL_INFO,  __VA_ARGS__)
#define QL_LOGD(...) ql_log(QL_LOG_LEVEL_DEBUG, __VA_ARGS__)

/* 子模块头文件放在末尾，保证上面这些定义先就位 */
#include "qltype_gen.h"
#include "qlmem.h"
#include "qlvalue.h"
#include "qlstr.h"
#include "qlevent.h"
#include "qlprop.h"
#include "qlclass.h"
#include "qlobject.h"
#include "qlgeom.h"
#include "qldisp.h"
#include "qlinput.h"
#include "qlanim.h"
#include "qlfont.h"
#include "qlfont_ft.h"
#include "qlimage.h"
#include "qlcanvas.h"
#include "qldraw2d.h"
#include "qlui.h"
#include "qluitext.h"
#include "qldisp_x11.h"
#include "qldisp_wayland.h"
#include "qlgfx_gl.h"

#endif /* QLGL_H */
