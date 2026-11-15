/**
 * @file    qlplatform.c
 * @brief   平台后端注册：应用覆盖 / 默认弱符号。
 */
#include <qlgl/qlgl.h>

static const QLPlatform *s_platform;

void qlgl_set_platform(const QLPlatform *platform)
{
    s_platform = platform;
}

const QLPlatform *qlgl_platform(void)
{
    if (s_platform != NULL)
        return s_platform;
    return qlgl_platform_default();
}

/*
 * 弱符号：桌面构建用 `ql_platform_desktop()`；MCU 板级文件定义自己的
 * `qlgl_platform_default()` 强符号即可覆盖，应用层仍然只调 `qlgl_init()`。
 *
 * 注意：放进静态库时，链接器只有在有人引用 `qlgl_platform_default` 时才拉这份
 * 目标文件；应用定义强符号时不会重复定义。
 */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
const QLPlatform *qlgl_platform_default(void)
{
    return ql_platform_desktop();
}
