/**
 * @file    qldl.c
 * @brief   动态加载包装（见 qldl.h）。
 */
#include <qlgl/qlgl.h>
#include <qlgl/qldl.h>

#if QL_CFG_HAVE_DLOPEN

#if defined(_WIN32)
#include <windows.h>
void *ql_dlopen(const char *name)
{
    if (name == NULL)
        return NULL;
    HMODULE h = LoadLibraryA(name);
    return (void *)h;
}
void *ql_dlsym(void *lib, const char *name)
{
    if (lib == NULL || name == NULL)
        return NULL;
    return (void *)(uintptr_t)GetProcAddress((HMODULE)lib, name);
}
void ql_dlclose(void *lib)
{
    if (lib != NULL)
        FreeLibrary((HMODULE)lib);
}
#else
#include <dlfcn.h>
void *ql_dlopen(const char *name)
{
    if (name == NULL)
        return NULL;
    return dlopen(name, RTLD_LAZY | RTLD_LOCAL);
}
void *ql_dlsym(void *lib, const char *name)
{
    if (lib == NULL || name == NULL)
        return NULL;
    return dlsym(lib, name);
}
void ql_dlclose(void *lib)
{
    if (lib != NULL)
        dlclose(lib);
}
#endif

#else /* QL_CFG_HAVE_DLOPEN == 0 */

void *ql_dlopen(const char *name)
{
    QL_UNUSED(name);
    return NULL;
}
void *ql_dlsym(void *lib, const char *name)
{
    QL_UNUSED(lib);
    QL_UNUSED(name);
    return NULL;
}
void ql_dlclose(void *lib)
{
    QL_UNUSED(lib);
}

#endif /* QL_CFG_HAVE_DLOPEN */
