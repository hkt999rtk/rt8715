/**************************************************************************//**
 * @file     rt_printf.h
 * @brief    This file defines the wrapper functions for printf API in ROM.
 * 
 * @version  V1.00
 * @date     2016-09-28
 *
 * @note
 *
 ******************************************************************************
 *
 * Copyright(c) 2007 - 2016 Realtek Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110, USA
 *
 ******************************************************************************/
 
#ifndef _RT_PRINTF__H
#define _RT_PRINTF__H

#ifdef  __cplusplus
extern "C" {
#endif
#include <cmsis_compiler.h>
#include <stdarg.h>
#include "basic_types.h"
#include "printf_entry.h"

#if defined(ROM_REGION)

int _rtl_printf(const char *fmt,...);
#if defined(CONFIG_VRF_MODE) && (CONFIG_VRF_MODE==1)
int _mutex_rtl_printf(osMutexId printlock,const char *fmt, ...);
#endif
int _rtl_sprintf(char *buf, const char *fmt,...);
int _rtl_snprintf(char *buf, size_t size, const char *fmt, ...);

int _xprintf(const char * fmt,...);
int _xsprintf(char *buf, const char * fmt,...);
int _xsnprintf(char *buf, size_t size, const char *fmt, ...);

#else   // else of "#if defined(ROM_REGION)"

#if defined(CONFIG_PLATFORM_8195BHP)

#if   defined ( __CC_ARM )                                            /* ARM Compiler 4/5 */
extern const stdio_printf_func_stubs_t Image$$_STUB_PRINTFS$$Base;     // symbol from linker script
#define __rom_stubs_stdprintf_s Image$$_STUB_PRINTFS$$Base
extern const stdio_printf_func_stubs_t Image$$_STUB_PRINTFNS$$Base;     // symbol from linker script
#define __rom_stubs_stdprintf_ns Image$$_STUB_PRINTFNS$$Base
#elif defined (__ARMCC_VERSION) && (__ARMCC_VERSION >= 6010050)       /* ARM Compiler 6 */
extern const stdio_printf_func_stubs_t Image$$_STUB_PRINTFS$$Base;     // symbol from linker script
#define __rom_stubs_stdprintf_s Image$$_STUB_PRINTFS$$Base
extern const stdio_printf_func_stubs_t Image$$_STUB_PRINTFNS$$Base;     // symbol from linker script
#define __rom_stubs_stdprintf_ns Image$$_STUB_PRINTFNS$$Base
#elif defined ( __GNUC__ )
extern const stdio_printf_func_stubs_t __rom_stubs_stdprintf_s;     // symbol from linker script
extern const stdio_printf_func_stubs_t __rom_stubs_stdprintf_ns;     // symbol from linker script
#elif defined ( __ICCARM__)
extern const stdio_printf_func_stubs_t __rom_stubs_stdprintf_s;     // symbol from linker script
extern const stdio_printf_func_stubs_t __rom_stubs_stdprintf_ns;     // symbol from linker script
#endif
#endif  // end of "#if defined(CONFIG_PLATFORM_8195BHP)"

extern const stdio_printf_func_stubs_t stdio_printf_stubs;     // symbol from linker script

// --- rt_printf callback types (runtime-swappable print backend) ---
typedef int (*rt_printf_fn_t)(const char *fmt, ...);
typedef int (*rt_sprintf_fn_t)(char *buf, const char *fmt, ...);
typedef int (*rt_snprintf_fn_t)(char *buf, size_t size, const char *fmt, ...);
typedef int (*rt_sscanf_fn_t)(const char *buf, const char *fmt, ...);

// Global callback pointers — defined in libc_wrap.c with platform-specific defaults:
//   LP  → ROM stubs (stdio_printf_stubs) to save RAM
//   HP  → newlib printf / sprintf / snprintf / sscanf
// Can be overridden at runtime (e.g. LP switching to IPC forwarding).
extern rt_printf_fn_t    _rt_printf_fn;
extern rt_sprintf_fn_t   _rt_sprintf_fn;
extern rt_snprintf_fn_t  _rt_snprintf_fn;
extern rt_printf_fn_t    _rt_printfl_fn;
extern rt_sprintf_fn_t   _rt_sprintfl_fn;
extern rt_snprintf_fn_t  _rt_snprintfl_fn;
extern rt_sscanf_fn_t    _rt_sscanf_fn;

// Unified macros — both LP and HP go through the same callback pointers
#define rt_printf(...)                  _rt_printf_fn(__VA_ARGS__)
#define rt_sprintf(buf, ...)            _rt_sprintf_fn(buf, __VA_ARGS__)
#define rt_snprintf(buf, size, ...)     _rt_snprintf_fn(buf, size, __VA_ARGS__)
#define rt_printfl(...)                 _rt_printfl_fn(__VA_ARGS__)
#define rt_sprintfl(buf, ...)           _rt_sprintfl_fn(buf, __VA_ARGS__)
#define rt_snprintfl(buf, size, ...)    _rt_snprintfl_fn(buf, size, __VA_ARGS__)
#if !defined(CONFIG_BUILD_SECURE)
#define rt_sscanf(buf, ...)             _rt_sscanf_fn(buf, __VA_ARGS__)
#endif

// rt_log_printf: LP needs the full log_buf support from ROM stubs;
// HP discards the plog argument and uses the printf callback directly.
#if defined(CONFIG_PLATFORM_8195BLP) && !defined(CONFIG_BUILD_SECURE)
#define rt_log_printf(plog, ...)        (stdio_printf_stubs.log_buf_printf(plog, __VA_ARGS__))
#elif defined(CONFIG_BUILD_SECURE)
#define rt_log_printf(plog, ...)
#else
#define rt_log_printf(plog, ...)        _rt_printf_fn(__VA_ARGS__)
#endif

#if !defined(CONFIG_BUILD_SECURE)
// Only Non-Secure region support printf to buffer
__STATIC_INLINE
void log_buf_show (log_buf_type_t *plog)
{
    stdio_printf_stubs.log_buf_show (plog);
}

__STATIC_INLINE
void log_buf_init (log_buf_type_t *plog)
{
    stdio_printf_stubs.log_buf_init (plog);
}

__STATIC_INLINE
void log_buf_set_msg_buf (log_buf_type_t *plog, char *pbuf, uint32_t buf_size)
{
    stdio_printf_stubs.log_buf_set_msg_buf (plog, pbuf, buf_size);
}
#endif  // end of "#if !defined(CONFIG_BUILD_SECURE)"

#endif  // end of "#if !defined(ROM_REGION)"

#ifdef  __cplusplus
}
#endif

#endif  // end of '#ifndef _RT_PRINTF__H'

