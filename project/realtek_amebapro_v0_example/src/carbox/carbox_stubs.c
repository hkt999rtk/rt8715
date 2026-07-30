/*
 * carbox_stubs.c — Stub implementations for symbols required by
 * pre-compiled BoxApp/CarPlay libraries that are not available
 * in the current SDK build.
 */

#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>

/* ------------------------------------------------------------------ */
/* assert — called directly by pre-compiled libs (not via macro)       */
/* ------------------------------------------------------------------ */
void assert(int expression)
{
    if (!expression) {
        printf("ASSERT failed\n");
        for (;;) vTaskDelay(1000);
    }
}

void __assert_func(const char *file, int line, const char *func,
                   const char *failedexpr)
{
    printf("ASSERT: %s:%d %s — %s\n", file, line, func, failedexpr);
    for (;;) vTaskDelay(1000);
}

void __assert(const char *file, int line, const char *failedexpr)
{
    printf("ASSERT: %s:%d — %s\n", file, line, failedexpr);
    for (;;) vTaskDelay(1000);
}

/* ------------------------------------------------------------------ */
/* _snprintf → snprintf  (MSVC -> GCC compat)                         */
/* ------------------------------------------------------------------ */
int _snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list args;
    int ret;
    va_start(args, fmt);
    ret = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return ret;
}

// /* ------------------------------------------------------------------ */
// /* strerrno                                                           */
// /* ------------------------------------------------------------------ */
// const char *strerrno(int errnum)
// {
//     return strerror(errnum);
// }

/* ------------------------------------------------------------------ */
/* f_putc / f_puts  — FatFs compat stubs                              */
/* ------------------------------------------------------------------ */
int f_putc(int c, void *fp) { (void)c; (void)fp; return 0; }
int f_puts(const char *str, void *fp) { (void)str; (void)fp; return 0; }