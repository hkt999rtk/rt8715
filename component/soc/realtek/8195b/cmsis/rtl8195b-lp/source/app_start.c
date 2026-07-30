/**************************************************************************//**
 * @file     app_start.c
 * @brief    The application entry function implementation. It initial the 
 *           application functions.
 * @version  V1.00
 * @date     2016-05-27
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

#undef ROM_REGION

#include "cmsis.h"


// LP platform: rt_printf callback variables (initialized at runtime)
// These are needed because LP does not compile libc_wrap.c.
#if defined(CONFIG_PLATFORM_8195BLP)
#include "stdio_port.h"
#include "rt_printf.h"

rt_printf_fn_t    _rt_printfl_fn   = NULL;
rt_sprintf_fn_t   _rt_sprintfl_fn  = NULL;
rt_snprintf_fn_t  _rt_snprintfl_fn = NULL;
rt_printf_fn_t    _rt_printf_fn    = NULL;
rt_sprintf_fn_t   _rt_sprintf_fn   = NULL;
rt_snprintf_fn_t  _rt_snprintf_fn  = NULL;
rt_sscanf_fn_t    _rt_sscanf_fn    = NULL;

static void rt_printf_init_lp(void)
{
    _rt_printfl_fn   = (rt_printf_fn_t)stdio_printf_stubs.rt_printfl;
    _rt_sprintfl_fn  = (rt_sprintf_fn_t)stdio_printf_stubs.rt_sprintfl;
    _rt_snprintfl_fn = (rt_snprintf_fn_t)stdio_printf_stubs.rt_snprintfl;
#if defined(CONFIG_BUILD_SECURE)
    _rt_printf_fn    = (rt_printf_fn_t)stdio_printf_stubs.rt_printfl;
    _rt_sprintf_fn   = (rt_sprintf_fn_t)stdio_printf_stubs.rt_sprintfl;
    _rt_snprintf_fn  = (rt_snprintf_fn_t)stdio_printf_stubs.rt_snprintfl;
#else
    _rt_printf_fn    = (rt_printf_fn_t)stdio_printf_stubs.rt_printf;
    _rt_sprintf_fn   = (rt_sprintf_fn_t)stdio_printf_stubs.rt_sprintf;
    _rt_snprintf_fn  = (rt_snprintf_fn_t)stdio_printf_stubs.rt_snprintf;
    _rt_sscanf_fn    = (rt_sscanf_fn_t)stdio_printf_stubs.rt_sscanf;
#endif
}
#endif // CONFIG_PLATFORM_8195BLP
extern void shell_cmd_init (void);
extern s32 shell_task(void);
extern void main(void);

void app_start (void) __attribute__ ((noreturn));

#if !defined ( __CC_ARM )                                            /* ARM Compiler 4/5 */
// for __CC_ARM compiler, it will add a __ARM_use_no_argv symbol for every main() function, that cause linker report error 
/// default main
__WEAK void main (void)
{
    dbg_printf ("Build @ %s, %s\r\n", __TIME__, __DATE__);
    shell_cmd_init ();
    while (1) {
        shell_task ();
    }
}
#endif

#if defined(__GNUC__)
extern uint32_t __HeapLimit[];
void * _sbrk (int incr)
{ 
   extern char   end; /* Set by linker.  */
   static char * heap_end; 
   char *        prev_heap_end; 

   if (heap_end == 0)
     heap_end = & end; 

   prev_heap_end = heap_end; 
   dbg_printf("sbrk %x %x %x\n\r", heap_end, incr, __HeapLimit);
   if(heap_end+incr > __HeapLimit){
	   dbg_printf("out of boundary %x\n\r", heap_end+incr);
	   return (void*)-1;
   }
   
   heap_end += incr; 

   return (void *) prev_heap_end; 
} 
#endif

void app_start (void)
{
#if defined(CONFIG_PLATFORM_8195BLP)
	rt_printf_init_lp();
#endif
    dbg_printf ("Build @ %s, %s\r\n", __TIME__, __DATE__);
    #if !defined(CONFIG_DISABLE_LP_SHELL)
	// disable shelll
	shell_cmd_init ();
	#endif
    main();
    while(1);
}


