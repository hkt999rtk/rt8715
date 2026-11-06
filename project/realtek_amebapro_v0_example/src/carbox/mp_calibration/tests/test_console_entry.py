"""编译实际命令handler，验证查询与拒绝路径；不复制产品解析逻辑。"""
from pathlib import Path
import subprocess,tempfile
root=next(p for p in Path(__file__).resolve().parents if (p/'component').is_dir())
def function(path,start,end):
 s=(root/path).read_text();return s[s.index(start):s.index(end,s.index(start))]
at=function('project/realtek_amebapro_v0_example/src/carbox/mp_calibration/recovery_atcmd_sys.c','void at_mp(void *arg)','\n#endif')
handler=function('component/soc/realtek/8195b/misc/driver/log_service.c','int mp_commnad_handler(char *cmd)','\n#else')
pre=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
typedef int CI2C;
static int active, queries, entries, driver_calls, resets;
static char output[4096];
static int capture(const char *fmt,...){va_list ap;va_start(ap,fmt);int n=vsnprintf(output+strlen(output),sizeof(output)-strlen(output),fmt,ap);va_end(ap);return n;}
#define printf capture
#define AT_PRINTK capture
#define MP_CALIBRATION_NOT_READY (-1001)
static void recovery_mp_print_status(void){queries++;}
static int recovery_mp_request(void){entries++;return 0;}
static int recovery_mp_is_active(void){return active;}
static const char *recovery_mp_phase_name(void){return active?"MP":"OTA";}
static void sys_reset(void){resets++;}
static void usbh_mp_init(void){}
static void usbh_mp_deinit(void){}
static CI2C *CI2C_new(void){return NULL;}
static int CI2C_Open(CI2C *x,char *p){(void)x;(void)p;return 0;}
static int CI2C_Read(CI2C *x,int a,unsigned char *b,int c,unsigned char *d,int e){(void)x;(void)a;(void)b;(void)c;(void)d;(void)e;return 0;}
static void CI2C_Close(CI2C *x){(void)x;}
static void CI2C_delete(CI2C *x){(void)x;}
static void mp_report_stack(const char *p){(void)p;}
static int mp_calibration_command(const char *p,int n){(void)p;(void)n;driver_calls++;return 0;}
'''
test=r'''
int main(void){
 at_mp("STATUS\r\n");assert(queries==1&&entries==0&&resets==0);
 at_mp("STATUSBAD");assert(queries==1&&entries==0);
 assert(mp_commnad_handler("iwpriv mp_start")==0);
 assert(driver_calls==0&&strstr(output,"error=-1001 phase=OTA")&&strstr(output,"STOP TEST"));
 at_mp("4");assert(entries==1&&driver_calls==0);
 at_mp("4\rBAD");assert(entries==1);
 at_mp("-0");assert(resets==0);
 active=1;output[0]=0;
 assert(mp_commnad_handler("iwpriv mp_start")==0&&driver_calls==1);
 assert(!strstr(output,"command error"));
 at_mp("STATUS");assert(queries==3&&entries==1&&driver_calls==1);
 at_mp("3");assert(!strstr(output,"4: reboot"));
 return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.c';p.write_text(pre+at+handler+test)
 subprocess.run(['gcc','-std=c99','-Wall','-Wextra','-Werror',str(p),'-o',d+'/test'],check=True)
 subprocess.run([d+'/test'],check=True)
print('actual console handler tests PASS')
