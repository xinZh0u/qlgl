/**
 * @file    qlwebvtt.h
 * @brief   WebVTT 解析（`<track>` 用）。内部头文件，不安装。
 */
#ifndef QLGL_SRC_MEDIA_QLWEBVTT_H
#define QLGL_SRC_MEDIA_QLWEBVTT_H

#include <stddef.h>
#include <stdint.h>

/* 一条 cue；text 是**去掉标签/实体**后的纯文本，长度 text_len（不以 NUL 结尾） */
typedef void (*ql_vtt_cue_fn)(void *user, int64_t start_ms, int64_t end_ms,
                              const char *text, size_t text_len);

/*
 * 解析 WebVTT；成功返回 cue 条数，语法错误返回 -1。
 * 纯函数：不分配内存，文本通过回调交出去。
 */
int ql_vtt_parse(const char *text, size_t len, ql_vtt_cue_fn fn, void *user);

#endif /* QLGL_SRC_MEDIA_QLWEBVTT_H */
