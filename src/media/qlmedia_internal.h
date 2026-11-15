/**
 * @file    qlmedia_internal.h
 * @brief   媒体模块内部接缝：默认后端/设备的工厂函数（不安装）。
 */
#ifndef QLGL_SRC_MEDIA_INTERNAL_H
#define QLGL_SRC_MEDIA_INTERNAL_H

#include <qlgl/qlgl.h>

/* ffmpeg 解码后端（QL_FEAT_MEDIA_FFMPEG=1 时才有实现） */
const QLMediaBackend *ql_media_backend_ffmpeg(void);

/* ALSA 音频设备（dlopen libasound；运行期没有就返回一个打开必失败的 sink） */
const QLAudioSink *ql_audio_sink_alsa(void);

#endif /* QLGL_SRC_MEDIA_INTERNAL_H */
