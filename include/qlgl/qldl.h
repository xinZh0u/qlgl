/**
 * @file    qldl.h
 * @brief   极简的动态加载包装（dlopen / LoadLibrary），只给"可选第三方库"用。
 *
 * 为什么要有它：B8 的 HarfBuzz / fribidi 在**只有运行库、没有开发头文件**的
 * 机器上也要能用（用户的发行版往往就是这个状态）。走 dlopen 就能做到
 * "装了就用、没装就退回内建实现"，也不用让下游在链接期多一个 -lharfbuzz。
 *
 * MCU 裸机没有动态加载：`QL_CFG_HAVE_DLOPEN=0` 时三个函数是空实现。
 */
#ifndef QLGL_DL_H
#define QLGL_DL_H

#include <string.h>

#include "qlgl.h"

/* 打开一个共享库（失败返回 NULL，不打印日志 —— 调用方自己决定要不要说） */
void *ql_dlopen(const char *name);

/* 取符号（失败返回 NULL） */
void *ql_dlsym(void *lib, const char *name);

/* 关闭（NULL 或未启用时无操作） */
void ql_dlclose(void *lib);

/* 把符号地址取到函数指针里。
 *
 * 为什么不直接强转：ISO C 不允许"对象指针 ↔ 函数指针"互转（`-Wpedantic` 会报
 * 错），但 POSIX 明确保证 `dlsym` 返回的地址可以直接当函数指针用。用 memcpy
 * 转一次既没有 UB 告警，也保持与 POSIX 一致。 */
#define QL_DLSYM(lib, name, fnptr)                                       do {                                                                     void *ql_sym_ = ql_dlsym((lib), (name));                             memcpy(&(fnptr), &ql_sym_, sizeof(fnptr));                       } while (0)

#endif /* QLGL_DL_H */
