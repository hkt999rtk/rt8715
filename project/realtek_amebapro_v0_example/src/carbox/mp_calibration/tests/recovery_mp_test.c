/* 主机测试：故障注入覆盖一次性启动标记与模式边界，不执行射频命令。 */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdarg.h>
#define RECOVERY_MP_HOST_TEST 1
#include "../mp_calibration.h"
#define MP_ONCE_PATH "mp_once.test"
static int slot=2, fw1=0, ota=0, reset_count=0, init_result=0, run0=1, run1=0, promisc=0;
static unsigned SystemCoreClock=300000000;
static int command_ready=1, start_result, start_calls;
int mp_calibration_start(void){start_calls++;return start_result;}
int mp_calibration_is_ready(void){return command_ready;}
int mp_calibration_get_driver_state(struct mp_calibration_driver_state *s){memset(s,0,sizeof(*s));return 0;}
static int fs_fail_remove, fs_fail_write, fs_fail_open, busy_on_guard, guard_held;
static FILE *test_fopen(const char *p,const char *m){return fs_fail_open ? NULL : fopen(p,m);}
static int marker_exists(void){struct stat st;return stat(MP_ONCE_PATH,&st)==0?1:errno==ENOENT?0:-1;}
static void reset_guard_enter(void){guard_held=1;if(busy_on_guard)ota=1;}
static void reset_guard_exit(void){guard_held=0;}
static int test_remove(const char *s){return fs_fail_remove ? -1 : remove(s);}
static size_t test_fwrite(const void *p,size_t a,size_t b,FILE *f){return fs_fail_write?0:fwrite(p,a,b,f);}
static int recovery_mp_current_slot(void){return slot;}
static int recovery_mp_fw1_valid(void){return fw1;}
static int recovery_ota_is_busy(void){return ota;}
static void sys_reset(void){assert(guard_held);reset_count++;}
static void LwIP_Init(void){}
int mp_calibration_prepare(void){return init_result;}
int mp_calibration_shutdown(void){return 0;}
static int rltk_wlan_running(int n){return n?run1:run0;}
static int is_promisc_enabled(void){return promisc;}
#define RTW_MODE_STA 1
static int wifi_mode=RTW_MODE_STA;
static char last_log[512];
static int capture_log(const char *fmt, ...) {va_list ap;va_start(ap,fmt);int n=vsnprintf(last_log,sizeof(last_log),fmt,ap);va_end(ap);return n;}
#define rt_printf capture_log
#define fopen test_fopen
#define remove test_remove
#define fwrite test_fwrite
#include "../recovery_mp.c"
#undef fopen
#undef remove
#undef fwrite
static void clean(void){start_result=start_calls=0;command_ready=1;remove(MP_ONCE_PATH); phase=RECOVERY_BOOT;slot=2;fw1=ota=reset_count=init_result=run1=promisc=fs_fail_remove=fs_fail_write=0;fs_fail_open=busy_on_guard=guard_held=0;run0=1;SystemCoreClock=300000000;wifi_mode=RTW_MODE_STA;}
int main(void){
 clean(); assert(recovery_mp_boot_select()==0);assert(phase==RECOVERY_OTA);assert(recovery_mp_ota_allowed());
 slot=1;assert(recovery_mp_request()<0&&reset_count==0);slot=2;
 fw1=1;assert(recovery_mp_request()<0&&reset_count==0);fw1=0;
 ota=1;assert(recovery_mp_request()<0&&reset_count==0);ota=0;
 fs_fail_write=1;assert(recovery_mp_request()<0&&reset_count==0);fs_fail_write=0;
 assert(recovery_mp_request()==0&&reset_count==1);
 phase=RECOVERY_BOOT;assert(recovery_mp_boot_select()==1);assert(fopen(MP_ONCE_PATH,"rb")==NULL);
 assert(recovery_mp_start()==0);assert(start_calls==1);assert(recovery_mp_is_active());assert(!recovery_mp_ota_allowed());
 assert(recovery_mp_request()==0&&reset_count==1);
 assert(recovery_mp_allow_console("mptest"));assert(recovery_mp_allow_console("iwpriv mp_start"));assert(recovery_mp_allow_console("MP=3"));
 assert(!recovery_mp_allow_console("ATWA=ssid"));assert(!recovery_mp_allow_console("wifi_on"));
 phase=RECOVERY_BOOT;assert(recovery_mp_boot_select()==0);
 assert(!recovery_mp_is_active());
 assert(recovery_mp_request()==0);phase=RECOVERY_BOOT;fs_fail_remove=1;
 assert(recovery_mp_boot_select()<0&&!recovery_mp_is_active());fs_fail_remove=0;
 clean();FILE *f=fopen(MP_ONCE_PATH,"wb");fputs("bad marker",f);fclose(f);assert(recovery_mp_boot_select()==0);
 clean();assert(recovery_mp_boot_select()==0);assert(recovery_mp_request()==0);phase=RECOVERY_BOOT;slot=1;assert(recovery_mp_boot_select()<0);
 clean();assert(recovery_mp_boot_select()==0);assert(recovery_mp_request()==0);phase=RECOVERY_BOOT;assert(recovery_mp_boot_select()==1);run1=1;assert(recovery_mp_start()<0&&!recovery_mp_is_active());
 clean();phase=RECOVERY_MP_START;promisc=1;assert(recovery_mp_start()<0);
 clean();phase=RECOVERY_MP_START;SystemCoreClock=400000000;assert(recovery_mp_start()<0);
 clean();phase=RECOVERY_MP_START;start_result=-1;assert(recovery_mp_start()<0&&!recovery_mp_is_active());
 clean();phase=RECOVERY_MP_START;init_result=-1;assert(recovery_mp_start()<0);
 clean();phase=RECOVERY_MP_START;wifi_mode=3;assert(recovery_mp_start()<0);
 clean();assert(recovery_mp_boot_select()==0);assert(recovery_mp_request()==0);phase=RECOVERY_BOOT;fs_fail_open=1;
 assert(recovery_mp_boot_select()<0&&phase==RECOVERY_ERROR);fs_fail_open=0;
 clean();assert(recovery_mp_boot_select()==0);busy_on_guard=1;assert(recovery_mp_request()<0&&reset_count==0&&!guard_held);
 clean();assert(recovery_mp_boot_select()==0);recovery_mp_print_status();
 assert(strstr(last_log,"phase=OTA")&&strstr(last_log,"ready=0")&&phase==RECOVERY_OTA&&reset_count==0);
 assert(recovery_mp_request()==0);phase=RECOVERY_BOOT;assert(recovery_mp_boot_select()==1);assert(recovery_mp_start()==0);
 recovery_mp_print_status();assert(strstr(last_log,"phase=MP")&&strstr(last_log,"ready=1"));
 run1=1;recovery_mp_print_status();assert(strstr(last_log,"ready=0"));run1=0;
 promisc=1;recovery_mp_print_status();assert(strstr(last_log,"ready=0"));promisc=0;
 assert(strcmp(recovery_mp_phase_name(),"MP")==0);
 command_ready=0;recovery_mp_print_status();assert(strstr(last_log,"ready=0"));
 clean();assert(!recovery_mp_ota_allowed());phase=RECOVERY_ERROR;assert(!recovery_mp_ota_allowed());phase=RECOVERY_RESET;assert(!recovery_mp_ota_allowed());puts("recovery MP lifecycle tests PASS");return 0;
}
