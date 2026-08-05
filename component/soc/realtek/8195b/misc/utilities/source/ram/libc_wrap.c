/**************************************************************************//**
* @file     libc_wrap.c
* @brief    The wraper functions of ROM code to replace some of utility
*           functions in Compiler's Library.
* @version  V1.00
* @date     2018-08-15
*
* @note
*
******************************************************************************
*
* Copyright(c) 2007 - 2018 Realtek Corporation. All rights reserved.
*
* SPDX-License-Identifier: Apache-2.0
*
* Licensed under the Apache License, Version 2.0 (the License); you may
* not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an AS IS BASIS, WITHOUT
* WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
******************************************************************************/
#if defined(CONFIG_PLATFORM_8195BHP) || defined(CONFIG_PLATFORM_8195BLP)
#include "platform_conf.h"
#endif
#include "basic_types.h"
#include "memory.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <reent.h>

#ifndef ENOMEM
#define ENOMEM 12
#endif

/* for GNU C++ */
#if defined(__GNUC__)
void* __dso_handle = 0;
#endif

#if defined(CONFIG_CMSIS_FREERTOS_EN) && (CONFIG_CMSIS_FREERTOS_EN != 0)
/**************************************************
* FreeRTOS memory management functions's wrapper to replace 
* malloc/free/realloc of GCC Lib.
**************************************************/
//#include "FreeRTOS.h"
// pvPortReAlloc currently not defined in portalbe.h
extern void* pvPortReAlloc( void *pv,  size_t xWantedSize );
extern void *pvPortMalloc( size_t xWantedSize );
extern void vPortFree( void *pv );

static int checked_calloc_size(size_t n, size_t size, size_t *total)
{
	if ((n != 0U) && (size > ((size_t)-1 / n))) {
		return 0;
	}

	*total = n * size;
	return 1;
}

static void *freertos_calloc(size_t n, size_t size)
{
	size_t total;
	void *ptr;

	if (!checked_calloc_size(n, size, &total)) {
		return NULL;
	}

	ptr = pvPortMalloc(total);
	if (ptr != NULL) {
		rt_memset(ptr, 0, total);
	}
	return ptr;
}

static void set_reent_errno(struct _reent *reent, int error)
{
	if (reent != NULL) {
		reent->_errno = error;
	}
}

void* __wrap_malloc( size_t size )
{
	return pvPortMalloc(size);
}

void* __wrap_realloc( void *p, size_t size )
{
	return (void*)pvPortReAlloc(p, size);
}

void* __wrap_calloc( size_t n, size_t size )
{
	return freertos_calloc(n, size);
}

void __wrap_free( void *p )
{
	vPortFree(p);
}

/* Keep newlib's reentrant allocator entry points on the same FreeRTOS heap. */
void *__wrap__malloc_r(struct _reent *reent, size_t size)
{
	void *ptr = pvPortMalloc(size);

	if ((ptr == NULL) && (size != 0U)) {
		set_reent_errno(reent, ENOMEM);
	}
	return ptr;
}

void *__wrap__realloc_r(struct _reent *reent, void *ptr, size_t size)
{
	void *new_ptr = pvPortReAlloc(ptr, size);

	if ((new_ptr == NULL) && (size != 0U)) {
		set_reent_errno(reent, ENOMEM);
	}
	return new_ptr;
}

void *__wrap__calloc_r(struct _reent *reent, size_t n, size_t size)
{
	size_t total;
	void *ptr;

	if (!checked_calloc_size(n, size, &total)) {
		set_reent_errno(reent, ENOMEM);
		return NULL;
	}

	ptr = freertos_calloc(n, size);
	if ((ptr == NULL) && (total != 0U)) {
		set_reent_errno(reent, ENOMEM);
	}
	return ptr;
}

void __wrap__free_r(struct _reent *reent, void *ptr)
{
	(void)reent;
	vPortFree(ptr);
}

/* Newlib strdup() uses _malloc_r(), but free() is wrapped to vPortFree(). */
char *__wrap_strdup(const char *src)
{
	const char *p;
	char *dst;
	size_t len = 0;

	if (src == NULL) {
		return NULL;
	}

	for (p = src; *p != '\0'; p++) {
		len++;
	}

	dst = (char *)pvPortMalloc(len + 1U);
	if (dst == NULL) {
		return NULL;
	}

	rt_memcpy(dst, src, len);
	dst[len] = '\0';
	return dst;
}
#endif  //  #if defined(CONFIG_CMSIS_FREERTOS_EN) && (CONFIG_CMSIS_FREERTOS_EN != 0)

/**************************************************
* string and memory api wrap for compiler
*
**************************************************/

#if defined(CONFIG_PLATFORM_8195BHP) || defined(CONFIG_PLATFORM_8195BLP)
#include "stdio_port.h"
#include "rt_printf.h"

// ============================================================
// rt_printf callback variables — platform-specific defaults
// LP: ROM stubs (stdio_printf_stubs) → saves RAM (~46KB total)
// HP: newlib printf / sprintf / snprintf / sscanf
// Overridable at runtime (e.g. LP can switch to IPC forwarding)
// ============================================================

// HP / other platforms: use newlib standard functions
// (LP callback variables are defined in misc/platform/libc_wrap.c)
#if !defined(CONFIG_PLATFORM_8195BLP)
#include <stdio.h>
rt_printf_fn_t    _rt_printf_fn    = printf;
rt_sprintf_fn_t   _rt_sprintf_fn   = sprintf;
rt_snprintf_fn_t  _rt_snprintf_fn  = snprintf;
rt_printf_fn_t    _rt_printfl_fn   = printf;
rt_sprintf_fn_t   _rt_sprintfl_fn  = sprintf;
rt_snprintf_fn_t  _rt_snprintfl_fn = snprintf;
rt_sscanf_fn_t    _rt_sscanf_fn    = sscanf;
#endif

#ifdef CONFIG_ADD_LOG
extern void add_log(const char *message);
#endif

// ============================================================
// printf mutex — serialises UART output without disabling
// interrupts (unlike taskENTER_CRITICAL). Falls back to
// taskENTER_CRITICAL before printf_lock_init() is called, so
// early-boot output continues to work. Call printf_lock_init()
// after the scheduler starts to switch to mutex mode.
// ============================================================
static SemaphoreHandle_t printf_mutex = NULL;

void printf_lock_init(void)
{
	if (printf_mutex == NULL) {
		printf_mutex = xSemaphoreCreateMutex();
		configASSERT(printf_mutex != NULL);
	}
}

SemaphoreHandle_t printf_lock_handle(void)
{
	return printf_mutex;
}

/*
 * ISR-safe printf lock:
 * - Task context: use mutex (if initialised) or critical section (fallback).
 * - ISR context:  skip locking — the underlying UART putc path is
 *   single-byte and the worst case is interleaved characters, which is
 *   acceptable compared to a hard fault / assertion.
 */
static inline void printf_lock(void)
{
	/* Read ICSR VECTACTIVE to detect ISR context (same method as
	 * vPortEnterCritical in port.c). */
	uint32_t vectactive = (*(volatile uint32_t *)0xE000ED04) & 0x1FFU;

	/* ISR context — return immediately, no locking support here. */
	if (vectactive != 0) {
		return;
	}

	if (printf_mutex != NULL) {
		xSemaphoreTake(printf_mutex, portMAX_DELAY);
	} else {
		/* mutex not initialised yet (early boot / pre-scheduler) —
		 * fall back to critical section for compatibility.
		 * Safe because ISR path already returned above. */
		taskENTER_CRITICAL();
	}
}

static inline void printf_unlock(void)
{
	uint32_t vectactive = (*(volatile uint32_t *)0xE000ED04) & 0x1FFU;

	/* ISR context — printf_lock returned early, nothing to unlock. */
	if (vectactive != 0) {
		return;
	}

	if (printf_mutex != NULL) {
		xSemaphoreGive(printf_mutex);
	} else {
		taskEXIT_CRITICAL();
	}
}

static int _sputc_cr(void *arg, char c)
{
	if (c == '\n') {
		stdio_printf_stubs.stdio_port_sputc(arg, '\r');
	}
	stdio_printf_stubs.stdio_port_sputc(arg, c);

#ifdef CONFIG_ADD_LOG
	{
		static char _line[256];
		static int  _pos;

		if (_pos < (int)sizeof(_line) - 1) {
			_line[_pos++] = c;
		}
		if (c == '\n') {
			_line[_pos] = '\0';
			add_log(_line);
			_pos = 0;
		}
	}
#endif

	return 1;
}

int __wrap_printf(const char * fmt,...)
{
	int count;
	va_list list;

	printf_lock();

	va_start(list, fmt);	
#if defined(CONFIG_BUILD_SECURE)	
	count = stdio_printf_stubs.printf_corel(_sputc_cr, (void*)NULL, fmt, list);
#else
	count = stdio_printf_stubs.printf_core(_sputc_cr, (void*)NULL, fmt, list);
#endif
	va_end(list);

	printf_unlock();

	return count;
}

int __wrap_vprintf(const char *fmt, va_list args)
{
	int count;

	printf_lock();

#if defined(CONFIG_BUILD_SECURE)     
	count = stdio_printf_stubs.printf_corel(_sputc_cr, (void*)NULL, fmt, args);
#else
	count = stdio_printf_stubs.printf_core(_sputc_cr, (void*)NULL, fmt, args);
#endif

	printf_unlock();

	return count;	
}

/* bare-metal newlib does not support C99 %zu/%zd. */
static int is_format_conversion(char c)
{
	switch (c) {
	case 'd': case 'i': case 'o': case 'u': case 'x': case 'X':
	case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
	case 'a': case 'A': case 'c': case 's': case 'p': case 'n':
		return 1;
	default:
		return 0;
	}
}

static int format_needs_z_fix(const char *fmt)
{
	int in_conversion = 0;

	while (*fmt != '\0') {
		if (!in_conversion) {
			in_conversion = (*fmt == '%');
		} else if (*fmt == '%') {
			in_conversion = 0;
		} else if (*fmt == 'z') {
			return 1;
		} else if (is_format_conversion(*fmt)) {
			in_conversion = 0;
		}
		fmt++;
	}
	return 0;
}

static const char *prepare_fmt_z(const char *fmt, char *dst, size_t dst_size)
{
	size_t len = 0;
	size_t i;
	int in_conversion = 0;

	if (fmt == NULL) {
		return NULL;
	}
	if (!format_needs_z_fix(fmt)) {
		return fmt;
	}

	while (fmt[len] != '\0') {
		len++;
	}
	if ((len + 1U) > dst_size) {
		return NULL;
	}

	for (i = 0; i <= len; i++) {
		char c = fmt[i];

		if (!in_conversion) {
			in_conversion = (c == '%');
		} else if (c == '%') {
			in_conversion = 0;
		} else if (c == 'z') {
			c = 'l';
		} else if (is_format_conversion(c)) {
			in_conversion = 0;
		}
		dst[i] = c;
	}
	return dst;
}

/* vsprintf is NOT --wrap'd, so calling it directly hits newlib's original. */
extern int vsprintf(char *buf, const char *fmt, va_list args);

int __wrap_sprintf(char *buf, const char * fmt,...)
{
	int count;
	va_list list;
	char fix_fmt_buf[256];
	const char *fixed_fmt = prepare_fmt_z(fmt, fix_fmt_buf, sizeof(fix_fmt_buf));

	if (fixed_fmt == NULL) {
		return -1;
	}

	va_start(list, fmt);
	count = vsprintf(buf, fixed_fmt, list);
	va_end(list);

	return count;
}

/* Forward: newlib's real snprintf (bypassed by --wrap) */
extern int __real_vsnprintf(char *buf, size_t size, const char *fmt, va_list args);

static int wrapped_vsnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
	char fix_fmt_buf[256];
	const char *fixed_fmt = prepare_fmt_z(fmt, fix_fmt_buf, sizeof(fix_fmt_buf));
	va_list copy;
	int count;

	if (fixed_fmt == NULL) {
		return -1;
	}

	va_copy(copy, args);
	count = __real_vsnprintf(buf, size, fixed_fmt, copy);
	va_end(copy);
	if (count < 0) {
		return count;
	}

	/* Preserve C99's would-be length when this newlib reports truncation. */
	if ((size > 0U) && ((size_t)count >= size)) {
		int total;

		va_copy(copy, args);
		total = __real_vsnprintf(NULL, 0, fixed_fmt, copy);
		va_end(copy);
		if (total > count) {
			return total;
		}
	}
	return count;
}

int __wrap_snprintf(char *buf, size_t size, const char *fmt,...)
{
	int count;
	va_list list;

	va_start(list, fmt);
	count = wrapped_vsnprintf(buf, size, fmt, list);
	va_end(list);
	return count;
}

int __wrap_vsnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
	return wrapped_vsnprintf(buf, size, fmt, args);
}


int __wrap_asprintf(char **strp, const char *fmt, ...)
{
	va_list args;
	va_list copy;
	int len;
	char fix_fmt_buf[256];
	const char *fixed_fmt;

	if (strp == NULL || fmt == NULL) {
		return -1;
	}

	*strp = NULL;
	fixed_fmt = prepare_fmt_z(fmt, fix_fmt_buf, sizeof(fix_fmt_buf));
	if (fixed_fmt == NULL) {
		return -1;
	}
	va_start(args, fmt);
	va_copy(copy, args);

	/* Newlib semantics: size zero returns the required length. */
	len = __real_vsnprintf(NULL, 0, fixed_fmt, copy);
	va_end(copy);

	if (len < 0) {
		va_end(args);
		return -1;
	}

	*strp = pvPortMalloc((size_t)len + 1U);
	if (*strp == NULL) {
		va_end(args);
		return -1;
	}

	if (__real_vsnprintf(*strp, (size_t)len + 1U, fixed_fmt, args) < 0) {
		vPortFree(*strp);
		*strp = NULL;
		va_end(args);
		return -1;
	}

	va_end(args);
	return len;
}

int __wrap_vsprintf(char *buf, const char *fmt, va_list args)
{
    int count;
    stdio_buf_t pnt_buf;

    pnt_buf.pbuf = buf;
    pnt_buf.pbuf_lim = 0;
#if defined(CONFIG_BUILD_SECURE)
    count = stdio_printf_stubs.printf_corel(stdio_printf_stubs.stdio_port_bufputc,(void *)&pnt_buf, fmt, args);
#else
	count = stdio_printf_stubs.printf_core(stdio_printf_stubs.stdio_port_bufputc,(void *)&pnt_buf, fmt, args);
#endif
    *(pnt_buf.pbuf) = 0;

    return count;	
}

int __wrap_puts(const char *str)
{
	return __wrap_printf("%s\n", str);
}

/*
 * The Realtek wrappers traditionally redirect the mem* family to utility
 * stubs whose implementations reside in mask ROM.  Network and video
 * profiling on RTL8195B shows those ROM routines as major CPU hot spots,
 * especially for the full-frame copies made by WLAN, lwIP/NCM and AirPlay.
 *
 * These symbols are linked with --wrap.  Keep the public wrappers (and
 * therefore all existing call sites, including rtw_mem* through the FreeRTOS
 * OS-service table), but skip the ROM dispatch entirely.  memcpy uses the
 * board-measured Cortex-M33 implementation below; the other primitives retain
 * their newlib implementations.
 */
extern int __real_memcmp(const void *s1, const void *s2, size_t n);
extern void *__real_memmove(void *dest, const void *src, size_t n);
extern void *__real_memset(void *dest, int value, size_t n);

int __wrap_memcmp(const void *av, const void *bv, size_t len)
{
	return __real_memcmp(av, bv, len);
}

/*
 * RTL8195B Cortex-M33 memcpy.
 *
 * On-board DWT measurements at 350 MHz showed the aligned 1--4 KiB paths are
 * 26--32% faster than newlib.  Four-word LDM/STM transfers must begin on a
 * 16-byte boundary: merely aligning to four bytes caused every transfer from
 * an offset-2 buffer to cross a bus/cache boundary and lose about 14% versus
 * newlib.  The scalar prefix below is therefore required for performance, not
 * just correctness.
 *
 * Differently aligned pointers cannot use LDM/STM, because those instructions
 * require aligned addresses.  ARMv8-M scalar LDR/STR does support the DRAM
 * buffers used here at unaligned addresses, however, so that path copies eight
 * words per branch before handling its tail.  This retains the existing
 * unaligned access semantics while removing most of the loop overhead seen in
 * the WLAN RX, TCP/IP and AirPlay profiler samples.
 *
 * memcpy does not permit overlapping objects, so no overlap handling is
 * required.  Keep the complete routine in ITCM; otherwise XIP latency hides
 * the M33 bulk loop's benefit.
 */
__attribute__((section(".itcm.text.libc_memcpy_m33"),
	optimize("O3", "no-tree-loop-distribute-patterns")))
void *__wrap_memcpy(void *s1, const void *s2, size_t n)
{
	uint8_t *dst = (uint8_t *)s1;
	const uint8_t *src = (const uint8_t *)s2;
	void *result = s1;

	if ((((uintptr_t)dst ^ (uintptr_t)src) & 3U) != 0U) {
		while (n >= 32U) {
			uint32_t w0, w1, w2, w3;
			__asm volatile(
				"ldr %0, [%4, #0]\n\t"
				"ldr %1, [%4, #4]\n\t"
				"ldr %2, [%4, #8]\n\t"
				"ldr %3, [%4, #12]\n\t"
				"str %0, [%5, #0]\n\t"
				"str %1, [%5, #4]\n\t"
				"str %2, [%5, #8]\n\t"
				"str %3, [%5, #12]\n\t"
				: "=&r" (w0), "=&r" (w1), "=&r" (w2), "=&r" (w3)
				: "r" (src), "r" (dst)
				: "memory");
			__asm volatile(
				"ldr %0, [%4, #16]\n\t"
				"ldr %1, [%4, #20]\n\t"
				"ldr %2, [%4, #24]\n\t"
				"ldr %3, [%4, #28]\n\t"
				"str %0, [%5, #16]\n\t"
				"str %1, [%5, #20]\n\t"
				"str %2, [%5, #24]\n\t"
				"str %3, [%5, #28]\n\t"
				: "=&r" (w0), "=&r" (w1), "=&r" (w2), "=&r" (w3)
				: "r" (src), "r" (dst)
				: "memory");
			src += 32U;
			dst += 32U;
			n -= 32U;
		}
		while (n >= 4U) {
			uint32_t word;
			__asm volatile(
				"ldr %0, [%1], #4\n\t"
				"str %0, [%2], #4\n\t"
				: "=&r" (word), "+r" (src), "+r" (dst)
				:
				: "memory");
			n -= 4U;
		}
		while (n-- != 0U) {
			*dst++ = *src++;
		}
		return result;
	}

	while (n != 0U && ((uintptr_t)dst & 3U) != 0U) {
		*dst++ = *src++;
		--n;
	}

	while (n >= 4U && ((uintptr_t)dst & 15U) != 0U) {
		uint32_t word;
		__asm volatile(
			"ldr %0, [%1], #4\n\t"
			"str %0, [%2], #4\n\t"
			: "=&r" (word), "+r" (src), "+r" (dst)
			:
			: "memory");
		n -= 4U;
	}

	while (n >= 32U) {
		__asm volatile(
			"ldmia %1!, {r2-r5}\n\t"
			"stmia %0!, {r2-r5}\n\t"
			"ldmia %1!, {r2-r5}\n\t"
			"stmia %0!, {r2-r5}\n\t"
			: "+r" (dst), "+r" (src)
			:
			: "r2", "r3", "r4", "r5", "memory");
		n -= 32U;
	}

	while (n >= 4U) {
		uint32_t word;
		__asm volatile(
			"ldr %0, [%1], #4\n\t"
			"str %0, [%2], #4\n\t"
			: "=&r" (word), "+r" (src), "+r" (dst)
			:
			: "memory");
		n -= 4U;
	}

	while (n-- != 0U) {
		*dst++ = *src++;
	}
	return result;
}

void *__wrap_memmove (void *destaddr, const void *sourceaddr, unsigned length)
{
	return __real_memmove(destaddr, sourceaddr, length);
}

void *__wrap_memset(void *dst0, int val, size_t length)
{
	return __real_memset(dst0, val, length);
}
// define in AmebaPro utilites/include/strporc.h
// replace by linking command
#include "strproc.h"
char *__wrap_strcat(char *dest,  char const *src)
{
	return strcat(dest, src);
}

char *__wrap_strchr(const char *s, int c)
{
	return strchr(s, c);
}

int __wrap_strcmp(char const *cs, char const *ct)
{
	return strcmp(cs, ct);
}

int __wrap_strncmp(char const *cs, char const *ct, size_t count)
{
	return strncmp(cs, ct, count);
}

int __wrap_strnicmp(char const *s1, char const *s2, size_t len)
{
	return strnicmp(s1, s2, len);
}


char *__wrap_strcpy(char *dest, char const *src)
{
	return strcpy(dest, src);
}


char *__wrap_strncpy(char *dest, char const *src, size_t count)
{
	return strncpy(dest, src, count);
}


size_t __wrap_strlcpy(char *dst, char const *src, size_t s)
{
	return strlcpy(dst, src, s);
}


size_t __wrap_strlen(char const *s)
{
	return strlen(s);
}


size_t __wrap_strnlen(char const *s, size_t count)
{
	return strnlen(s, count);
}


char *__wrap_strncat(char *dest, char const *src, size_t count)
{
	return strncat(dest, src, count);
}

char *__wrap_strpbrk(char const *cs, char const *ct)
{
	return strpbrk(cs, ct);
}


size_t __wrap_strspn(char const *s, char const *accept)
{
	return strspn(s, accept);
}


char *__wrap_strstr(char const *s1, char const *s2)
{
	return strstr(s1, s2);
}


char *__wrap_strtok(char *s, char const *ct)
{
	return strtok(s, ct);
}


size_t __wrap_strxfrm(char *dest, const char *src, size_t n)
{
	return strxfrm(dest, src, n);
}

char *__wrap_strsep(char **s, const char *ct)
{
	return strsep(s, ct);
}

double __wrap_strtod(const char *str, char **endptr)
{
	return strtod(str, endptr);
}

float __wrap_strtof(const char *str, char **endptr)
{
	return strtof(str, endptr);
}


long double __wrap_strtold(const char *str, char **endptr)
{
	return strtold(str, endptr);
}

long __wrap_strtol(const char *nptr, char **endptr, int base)
{
	return strtol(nptr, endptr, base);
}


long long __wrap_strtoll(const char *nptr, char **endptr, int base)
{
	return strtoll(nptr, endptr, base);
}


unsigned long __wrap_strtoul(const char *nptr, char **endptr, int base)
{
	return strtoul(nptr, endptr, base);
}


unsigned long long __wrap_strtoull(const char *nptr, char **endptr, int base)
{
	return strtoull(nptr, endptr, base);
}

int __wrap_atoi(const char *num)
{
	return atoi(num);
}

unsigned int __wrap_atoui(const char *num)
{
	return atoui(num);
}

long __wrap_atol(const char *num)
{
	return atol(num);
}

unsigned long __wrap_atoul(const char *num)
{
	return atoul(num);
}


unsigned long long __wrap_atoull(const char *num)
{
	return atoull(num);
}


double __wrap_atof(const char *str)
{
	return atof(str);
}	

#endif // CONFIG_PLATFORM_8195BHP

void __wrap_abort(void)
{
	__wrap_printf("\n\rabort execution\n\r");
	while(1);
}

/**************************************************
* FILE api wrap for compiler
*
**************************************************/
#if 0 /* File APIs are dispatched by carbox/vfs_compat/vfs_wrap.c. */
//#if defined(CONFIG_FATFS_WRAPPER) && (CONFIG_FATFS_WRAPPER == 1)
/*
--redirect fopen=__wrap_fopen
--redirect fclose=__wrap_fclose
--redirect fread=__wrap_fread
--redirect fwrite=__wrap_fwrite
--redirect fseek=__wrap_fseek
--redirect fsetpos=__wrap_fsetpos
--redirect fgetpos=__wrap_fgetpos
--redirect rewind=__wrap_rewind
--redirect fflush=__wrap_fflush
--redirect remove=__wrap_remove
--redirect rename=__wrap_rename
--redirect feof=__wrap_feof
--redirect ferror=__wrap_ferror
--redirect ftell=__wrap_ftell
--redirect fputc=__wrap_fputc
--redirect fputs=__wrap_fputs
--redirect fgets=__wrap_fgets
*/


#include <stdio.h>
#include "fatfs_wrap.h"
#include "carbox_flash_layout.h"
#include "carbox_littlefs.h"

static const char *carbox_vfs_wrap_path(const char *path, char *translated, size_t translated_len)
{
	int ret;

	ret = carbox_vfs_translate_path(path, translated, translated_len);
	if (ret < 0) {
		return NULL;
	}

	return (ret > 0) ? translated : path;
}

typedef struct {
	carbox_littlefs_file_t *file;
	int error;
} carbox_lfs_stream_t;

typedef struct {
	carbox_littlefs_dir_t *dir;
	struct dirent entry;
} carbox_lfs_dir_stream_t;

static carbox_lfs_stream_t *carbox_lfs_stream(FILE *stream)
{
	return (carbox_lfs_stream_t *)stream;
}

static int carbox_lfs_mode_flags(const char *mode)
{
	int flags;
	if (mode == NULL || mode[0] == '\0') return -1;
	if (strchr(mode, '+') != NULL) flags = CARBOX_LFS_O_RDWR;
	else if (mode[0] == 'r') flags = CARBOX_LFS_O_RDONLY;
	else flags = CARBOX_LFS_O_WRONLY;
	if (mode[0] == 'w') flags |= CARBOX_LFS_O_CREAT | CARBOX_LFS_O_TRUNC;
	if (mode[0] == 'a') flags |= CARBOX_LFS_O_CREAT | CARBOX_LFS_O_APPEND;
	if (strchr(mode, 'x') != NULL) flags |= CARBOX_LFS_O_EXCL;
	return flags;
}

FILE *__wrap_fopen(const char *filename, const char *mode)
{
	char translated[CARBOX_VFS_COMPAT_PATH_MAX];
	const char *path = carbox_vfs_wrap_path(filename, translated, sizeof(translated));
	carbox_lfs_stream_t *stream;
	int flags = carbox_lfs_mode_flags(mode);
	if (path == NULL || flags < 0) return NULL;
	stream = pvPortMalloc(sizeof(*stream));
	if (stream == NULL) return NULL;
	stream->error = 0;
	stream->file = carbox_littlefs_open(path, flags);
	if (stream->file == NULL) {
		vPortFree(stream);
		return NULL;
	}
	return (FILE *)stream;
}

int __wrap_fclose(FILE *stream)
{
	carbox_lfs_stream_t *lfs_stream = carbox_lfs_stream(stream);
	int ret;
	if (lfs_stream == NULL) return EOF;
	ret = carbox_littlefs_close(lfs_stream->file);
	vPortFree(lfs_stream);
	return ret == 0 ? 0 : EOF;
}

size_t __wrap_fread(void *ptr, size_t size, size_t count, FILE *stream)
{
	carbox_lfs_stream_t *lfs_stream = carbox_lfs_stream(stream);
	int ret;
	if (lfs_stream == NULL || ptr == NULL || size == 0U || count == 0U || count > (size_t)-1 / size) return 0;
	ret = carbox_littlefs_read(lfs_stream->file, ptr, size * count);
	if (ret < 0) { lfs_stream->error = 1; return 0; }
	return (size_t)ret / size;
}

size_t __wrap_fwrite(const void *ptr, size_t size, size_t count, FILE *stream)
{
	carbox_lfs_stream_t *lfs_stream = carbox_lfs_stream(stream);
	int ret;
	if (lfs_stream == NULL || ptr == NULL || size == 0U || count == 0U || count > (size_t)-1 / size) return 0;
	ret = carbox_littlefs_write(lfs_stream->file, ptr, size * count);
	if (ret < 0) { lfs_stream->error = 1; return 0; }
	return (size_t)ret / size;
}

int __wrap_fseek(FILE *stream, long int offset, int origin)
{
	carbox_lfs_stream_t *lfs_stream = carbox_lfs_stream(stream);
	int ret;
	if (lfs_stream == NULL || (long int)(int32_t)offset != offset) return -1;
	ret = carbox_littlefs_seek(lfs_stream->file, (int32_t)offset, origin);
	if (ret < 0) { lfs_stream->error = 1; return -1; }
	return 0;
}

void __wrap_rewind(FILE *stream) { (void)__wrap_fseek(stream, 0, SEEK_SET); }

int __wrap_fflush(FILE *stream)
{
	carbox_lfs_stream_t *lfs_stream;
	if (stream == stdout || stream == stderr || stream == NULL) return 0;
	lfs_stream = carbox_lfs_stream(stream);
	return carbox_littlefs_sync(lfs_stream->file) == 0 ? 0 : EOF;
}

int __wrap_remove(const char *filename)
{
	char translated[CARBOX_VFS_COMPAT_PATH_MAX];
	const char *path = carbox_vfs_wrap_path(filename, translated, sizeof(translated));
	return path != NULL && carbox_littlefs_remove(path) == 0 ? 0 : -1;
}

int __wrap_rename(const char *oldname, const char *newname)
{
	char old_path[CARBOX_VFS_COMPAT_PATH_MAX], new_path[CARBOX_VFS_COMPAT_PATH_MAX];
	const char *old_lfs = carbox_vfs_wrap_path(oldname, old_path, sizeof(old_path));
	const char *new_lfs = carbox_vfs_wrap_path(newname, new_path, sizeof(new_path));
	return old_lfs != NULL && new_lfs != NULL && carbox_littlefs_rename(old_lfs, new_lfs) == 0 ? 0 : -1;
}

int __wrap_feof(FILE *stream)
{
	carbox_lfs_stream_t *lfs_stream = carbox_lfs_stream(stream);
	int pos = carbox_littlefs_tell(lfs_stream->file);
	int size = carbox_littlefs_size(lfs_stream->file);
	return pos >= 0 && size >= 0 && pos >= size;
}

int __wrap_ferror(FILE *stream) { return carbox_lfs_stream(stream)->error; }
long int __wrap_ftell(FILE *stream) { return carbox_littlefs_tell(carbox_lfs_stream(stream)->file); }

#include "stdio_port_func.h"

/* FreeRTOS critical section for atomic UART output */

int __wrap_fputc ( int character, FILE * stream )
{
	if(stream == stdout || stream == stderr){
		printf_lock();
		stdio_port_putc(character);
		if(character=='\n')
			stdio_port_putc('\r');
		printf_unlock();
		return character;
	}
	
	{
		unsigned char byte = (unsigned char)character;
		return __wrap_fwrite(&byte, 1, 1, stream) == 1U ? character : EOF;
	}
}

int __wrap_fputs ( const char * str, FILE * stream )
{
	size_t len = strlen(str);
	return __wrap_fwrite(str, 1, len, stream) == len ? (int)len : EOF;
}

char * __wrap_fgets ( char * str, int num, FILE * stream )
{
	int pos = 0;
	if (str == NULL || num <= 1) return NULL;
	while (pos < num - 1) {
		if (__wrap_fread(&str[pos], 1, 1, stream) != 1U) break;
		pos++;
		if (str[pos - 1] == '\n') break;
	}
	if (pos == 0) return NULL;
	str[pos] = '\0';
	return str;
}

DIR *__wrap_opendir(const char *name)
{
	char translated[CARBOX_VFS_COMPAT_PATH_MAX];
	const char *path = carbox_vfs_wrap_path(name, translated, sizeof(translated));
	carbox_lfs_dir_stream_t *stream;
	if (path == NULL) return NULL;
	stream = pvPortMalloc(sizeof(*stream));
	if (stream == NULL) return NULL;
	memset(stream, 0, sizeof(*stream));
	stream->dir = carbox_littlefs_opendir(path);
	if (stream->dir == NULL) { vPortFree(stream); return NULL; }
	return (DIR *)stream;
}

struct dirent *__wrap_readdir(DIR *pdir)
{
	carbox_lfs_dir_stream_t *stream = (carbox_lfs_dir_stream_t *)pdir;
	int is_dir, ret;
	uint32_t size;
	ret = carbox_littlefs_readdir(stream->dir, stream->entry.d_name,
							 sizeof(stream->entry.d_name), &is_dir, &size);
	if (ret <= 0) return NULL;
	stream->entry.d_ino = 0;
	stream->entry.d_off = 0;
	stream->entry.d_reclen = (unsigned short)size;
	stream->entry.d_namlen = strlen(stream->entry.d_name);
	stream->entry.d_type = is_dir ? DT_DIR : DT_REG;
	return &stream->entry;
}

int __wrap_closedir(DIR *dirp)
{
	carbox_lfs_dir_stream_t *stream = (carbox_lfs_dir_stream_t *)dirp;
	int ret = carbox_littlefs_closedir(stream->dir);
	vPortFree(stream);
	return ret == 0 ? 0 : -1;
}

int __wrap_scandir(const char *dirp, struct dirent ***namelist,
				   int (*filter)(const struct dirent *),
				   int (*compar)(const struct dirent **, const struct dirent **))
{
	DIR *m_dir;
	struct dirent *entry;
	struct dirent **list = NULL;
	int count = 0;
	int capacity = 0;
	int failed = 0;
	
	if (namelist == NULL) return -1;
	*namelist = NULL;
	m_dir = __wrap_opendir(dirp);
	if (m_dir == NULL) return -1;

	for (;;) {
		entry = __wrap_readdir(m_dir);
		if (entry == NULL) break;
		if (filter != NULL && !filter(entry)) continue;
		if (count == capacity) {
			int new_capacity = capacity == 0 ? 8 : capacity * 2;
			struct dirent **new_list;
			if (new_capacity < capacity || (size_t)new_capacity > (size_t)-1 / sizeof(*list)) {
				failed = 1;
				break;
			}
			new_list = list == NULL ? __wrap_malloc((size_t)new_capacity * sizeof(*list)) :
					__wrap_realloc(list, (size_t)new_capacity * sizeof(*list));
			if (new_list == NULL) {
				failed = 1;
				break;
			}
			list = new_list;
			capacity = new_capacity;
		}
		list[count] = __wrap_malloc(sizeof(*list[count]));
		if (list[count] == NULL) {
			failed = 1;
			break;
		}
		memcpy(list[count], entry, sizeof(*list[count]));
		count++;
	}

	if (__wrap_closedir(m_dir) < 0) failed = 1;
	if (failed) {
		while (count > 0) vPortFree(list[--count]);
		vPortFree(list);
		return -1;
	}
	if (compar != NULL) qsort(list, count, sizeof(*list), (int (*)(const void *, const void *))compar);
	*namelist = list;
	return count;
}

int __wrap_rmdir(const char *path) 
{
	return __wrap_remove(path);
}

int __wrap_mkdir(const char *pathname, mode_t mode)
{
	char translated[CARBOX_VFS_COMPAT_PATH_MAX];
	const char *path = carbox_vfs_wrap_path(pathname, translated, sizeof(translated));
	(void)mode;
	return path != NULL && carbox_littlefs_mkdir(path) == 0 ? 0 : -1;
}

int __wrap_access(const char *pathname, int mode)
{
	char translated[CARBOX_VFS_COMPAT_PATH_MAX];
	const char *path = carbox_vfs_wrap_path(pathname, translated, sizeof(translated));
	uint32_t size;
	int is_dir;
	(void)mode;
	return path != NULL && carbox_littlefs_stat(path, &size, &is_dir) == 0 ? 0 : -1;
}

int __wrap_stat(const char *path, struct stat *buf)
{
	char translated[CARBOX_VFS_COMPAT_PATH_MAX];
	const char *lfs_path = carbox_vfs_wrap_path(path, translated, sizeof(translated));
	uint32_t size;
	int is_dir;
	if (lfs_path == NULL || buf == NULL || carbox_littlefs_stat(lfs_path, &size, &is_dir) != 0) return -1;
	memset(buf, 0, sizeof(*buf));
	buf->st_mode = (is_dir ? S_IFDIR : S_IFREG) | 0777;
	buf->st_size = size;
	buf->st_blksize = CARBOX_FLASH_SECTOR_SIZE;
	buf->st_blocks = (size + 511U) / 512U;
	return 0;
}
//#endif
#endif

#if defined(__GNUC__)
#include <errno.h>

static int gnu_errno;
volatile int * __aeabi_errno_addr (void)
{
	return &gnu_errno;
}
#endif
