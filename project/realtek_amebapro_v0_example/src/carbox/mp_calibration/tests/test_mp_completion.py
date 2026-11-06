"""执行实际启动回覆区块：只有 MP 初始化成功才能给仪器 MEM 完成标志。"""
from pathlib import Path
import subprocess, tempfile
root=next(p for p in Path(__file__).resolve().parents if (p/'component').is_dir())
s=(root/'component/soc/realtek/8195b/misc/driver/log_service.c').read_text()
start=s.index('#if CARBOX_RECOVERY',s.index('void log_service(void *param)'))
block=s[start:s.index('#endif',start)+len('#endif')]
pre=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#define CARBOX_RECOVERY 1
static int active,ready;
static char output[2048];
static int recovery_mp_is_active(void){return active;}
static int mp_calibration_is_ready(void){return ready;}
static unsigned int xPortGetFreeHeapSize(void){return 12345;}
static void at_printf(const char *fmt,...){va_list ap;va_start(ap,fmt);vsnprintf(output+strlen(output),sizeof(output)-strlen(output),fmt,ap);va_end(ap);}
static void reply(void){
'''
test=r'''
}
int main(void){
 for(active=0;active<=1;active++) for(ready=0;ready<=1;ready++){
  output[0]=0; reply();
  if(active && ready){
   assert(strstr(output,"[REC_MP] READY"));
   assert(strstr(output,"MP=4: OK"));
   assert(strstr(output,"[MEM] After do cmd, available heap 12345"));
   assert(strstr(output,"# "));
  }else assert(!strstr(output,"[MEM]") && !strstr(output,"MP=4: OK"));
 }
 return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.c';p.write_text(pre+block+test)
 subprocess.run(['gcc','-std=c99','-Wall','-Wextra',str(p),'-o',d+'/test'],check=True)
 subprocess.run([d+'/test'],check=True)
print('MP completion reply tests PASS')
