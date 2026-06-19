/* ARET HLE runtime — Win32 → POSIX shims. See aret_hle.h for the calling model.
 *
 * Every shim receives the modelled stack pointer `esp`; the original stdcall
 * arguments are the consecutive 32-bit words at esp+0, esp+4, … We implement
 * each Windows call in terms of the native POSIX/libc of the target, so the
 * recompiled program runs natively on Linux. */

#include "aret_hle.h"

#include <unistd.h>
#include <stdlib.h>

/* Read stdcall argument `i` (a 32-bit word) from the modelled stack. */
static inline uint32_t arg(uint32_t esp, int i) {
    return ((const uint32_t *)(uintptr_t)esp)[i];
}

/* Per-thread Win32 last-error code (good enough single-threaded; M1 scope). */
static uint32_t g_last_error = 0;

/* Map a Windows standard-handle constant to a POSIX file descriptor. */
static int std_fd(uint32_t nStdHandle) {
    switch ((int32_t)nStdHandle) {
        case -10: return 0; /* STD_INPUT_HANDLE  */
        case -11: return 1; /* STD_OUTPUT_HANDLE */
        case -12: return 2; /* STD_ERROR_HANDLE  */
        default:  return (int)nStdHandle; /* already a fd-like handle */
    }
}

uint32_t GetStdHandle(uint32_t esp) {
    return (uint32_t)std_fd(arg(esp, 0));
}

/* Common implementation for WriteFile / WriteConsoleA: (h, buf, count, *done). */
static uint32_t write_common(uint32_t esp) {
    int fd = std_fd(arg(esp, 0));
    const void *buf = (const void *)(uintptr_t)arg(esp, 1);
    uint32_t count = arg(esp, 2);
    uint32_t pdone = arg(esp, 3);

    ssize_t n = write(fd, buf, count);
    if (n < 0) {
        g_last_error = 5; /* ERROR_ACCESS_DENIED, approximate */
        if (pdone) *(uint32_t *)(uintptr_t)pdone = 0;
        return 0; /* FALSE */
    }
    if (pdone) *(uint32_t *)(uintptr_t)pdone = (uint32_t)n;
    return 1; /* TRUE */
}

uint32_t WriteFile(uint32_t esp)     { return write_common(esp); }
uint32_t WriteConsoleA(uint32_t esp) { return write_common(esp); }

uint32_t ReadFile(uint32_t esp) {
    int fd = std_fd(arg(esp, 0));
    void *buf = (void *)(uintptr_t)arg(esp, 1);
    uint32_t count = arg(esp, 2);
    uint32_t pdone = arg(esp, 3);

    ssize_t n = read(fd, buf, count);
    if (n < 0) {
        g_last_error = 5;
        if (pdone) *(uint32_t *)(uintptr_t)pdone = 0;
        return 0;
    }
    if (pdone) *(uint32_t *)(uintptr_t)pdone = (uint32_t)n;
    return 1;
}

uint32_t ExitProcess(uint32_t esp) {
    exit((int)arg(esp, 0));
    /* not reached */
    return 0;
}

uint32_t GetLastError(uint32_t esp) {
    (void)esp;
    return g_last_error;
}

uint32_t SetLastError(uint32_t esp) {
    g_last_error = arg(esp, 0);
    return 0;
}

uint32_t Sleep(uint32_t esp) {
    uint32_t ms = arg(esp, 0);
    usleep((useconds_t)ms * 1000u);
    return 0;
}

uint32_t GetCurrentProcessId(uint32_t esp) {
    (void)esp;
    return (uint32_t)getpid();
}
