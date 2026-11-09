/**
 * @file    syscalls.c
 * @brief   最小 syscall 层：堆用静态池，输出走 ARM 半主机（semihosting）。
 *
 * 这两样正是 MCU 上最需要"显式声明"的东西：
 *   - 内存：_sbrk 从一个静态数组里切（不用链接器脚本里的堆也行）；
 *   - 输出：printf → _write → semihosting（QEMU 用 `-semihosting` 接住）。
 */
#include <stddef.h>
#include <stdint.h>

#define HEAP_BYTES (48u * 1024u)
static uint8_t s_heap[HEAP_BYTES];
static size_t s_used;

void *_sbrk(ptrdiff_t inc)
{
    if (s_used + (size_t)inc > HEAP_BYTES)
        return (void *)-1;
    void *p = &s_heap[s_used];
    s_used += (size_t)inc;
    return p;
}

/* ARM 半主机：SYS_WRITE0 = 0x04，r0 = 操作码，r1 = 字符串 */
static int semihost_write0(const char *s)
{
    register int r0 __asm("r0") = 0x04;
    register const char *r1 __asm("r1") = s;
    __asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}

int _write(int fd, const char *buf, int len)
{
    (void)fd;
    static char tmp[256];
    int n = 0;
    for (int i = 0; i < len; i++) {
        if (buf[i] == '\n' || n >= (int)sizeof(tmp) - 1) {
            tmp[n] = '\0';
            semihost_write0(tmp);
            n = 0;
            if (buf[i] == '\n')
                semihost_write0("\n");
            continue;
        }
        tmp[n++] = buf[i];
    }
    if (n > 0) {
        tmp[n] = '\0';
        semihost_write0(tmp);
    }
    return len;
}

/* ARM 半主机 SYS_EXIT = 0x18：ADP_Stopped_ApplicationExit = 0x20026 */
void _exit(int code)
{
    register int r0 __asm("r0") = 0x18;
    register uint32_t r1 __asm("r1") = 0x20026u + ((uint32_t)code << 16);
    __asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    (void)r1;
    for (;;)
        __asm volatile("wfi");
}

int _close(int fd) { (void)fd; return -1; }
int _lseek(int fd, int off, int dir) { (void)fd; (void)off; (void)dir; return -1; }
int _read(int fd, char *buf, int len) { (void)fd; (void)buf; (void)len; return -1; }
int _fstat(int fd, void *st) { (void)fd; (void)st; return -1; }
int _isatty(int fd) { (void)fd; return 1; }
int _kill(int pid, int sig) { (void)pid; (void)sig; return -1; }
int _getpid(void) { return 1; }
