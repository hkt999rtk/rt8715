"""编译实际MP模块，以假driver验证返回0但未完成初始化、重复start、stop和状态丢失。"""
from pathlib import Path
import re, subprocess, tempfile
base=Path(__file__).resolve().parents[1]
src=base
code=(src/'mp_calibration.c').read_text()
code=re.sub(r'^#include [<"](?:autoconf.h|wifi_conf.h|wifi_util.h|wlan_intf.h)[>"]\n','',code,flags=re.M)
pre=r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#define CONFIG_MP_INCLUDED 1
#define CARBOX_MP_DRIVER_LAYOUT_20261106 1
#define MP_CALIBRATION_HOST_TEST 1
#define RTW_MODE_STA 1
typedef int rtw_mode_t;
rtw_mode_t wifi_mode;
static int run[2], promisc, calls, starts, response, incomplete;
static void driver_state(unsigned mode,unsigned fw,unsigned state);
int rltk_wlan_running(int i){return run[i];}
int is_promisc_enabled(void){return promisc;}
int wifi_on(int m){wifi_mode=m;run[0]=1;return 0;}
int wifi_off(void){run[0]=run[1]=0;driver_state(0,0,0);return 0;}
int wext_private_command(const char *iface,char *cmd,int show){
 (void)show;assert(!strcmp(iface,"wlan0"));calls++;
 if(!strcmp(cmd,"mp_start")){starts++;if(!response&&!incomplete)driver_state(1,0x10081,1);}
 if(!strcmp(cmd,"mp_stop"))driver_state(0,0,0);
 return response;
}
'''
test=r'''
struct mp_wlan_entry rltk_wlan_info[2];
static uint32_t storage[0x4000/4];
static unsigned char netdev[64];
static void *adapter=storage;
static void driver_state(unsigned mode,unsigned fw,unsigned state){
 ((unsigned char *)storage)[0x18a7]=(unsigned char)mode;
 storage[0x10/4]=fw;storage[0x31bc/4]=state;
}
int main(void){
 void *priv=&adapter;
 memcpy(netdev+16,&priv,sizeof(priv));
 rltk_wlan_info[0].netdev=netdev;rltk_wlan_info[0].flag=1;
 assert(mp_calibration_prepare()==0);
 assert(!mp_calibration_is_ready());
 assert(mp_calibration_command("iwpriv mp_channel 36",1)==MP_CALIBRATION_NOT_READY&&calls==0);
 incomplete=1;assert(mp_calibration_start()==MP_CALIBRATION_DRIVER_STATE&&!mp_calibration_is_ready());
 response=-7;assert(mp_calibration_start()==-7&&!mp_calibration_is_ready());response=0;incomplete=0;
 assert(mp_calibration_start()==0&&mp_calibration_is_ready());
 int saved=starts;
 assert(mp_calibration_command("iwpriv mp_start\r\n",1)==0&&starts==saved);
 assert(mp_calibration_command("iwpriv mp_channel 36",1)==0);
 assert(mp_calibration_command("iwpriv mp_stop",1)==0&&!mp_calibration_is_ready());
 int count=calls;
 assert(mp_calibration_command("mp_arx start",1)==MP_CALIBRATION_NOT_READY&&calls==count);
 assert(mp_calibration_command("mp_query",1)==0);
 assert(mp_calibration_command("mp_start",1)==0&&starts==saved+1);
 driver_state(1,0x80,1);assert(!mp_calibration_is_ready());
 assert(mp_calibration_command("mp_txpower patha=40,pathb=40",1)==MP_CALIBRATION_NOT_READY);
 driver_state(1,0x10081,0);assert(!mp_calibration_is_ready());
 driver_state(1,0x10081,7);assert(mp_calibration_is_ready()); /* TX状态不必恒为1 */
 adapter=NULL;assert(!mp_calibration_is_ready());adapter=storage;
 assert(mp_calibration_command("mp_start\nmp_ctx background,pkt",1)==MP_CALIBRATION_BAD_COMMAND);
 promisc=1;assert(mp_calibration_command("mp_query",1)==MP_CALIBRATION_NOT_READY);promisc=0;
 assert(!mp_calibration_is_ready()); /* 环境错误锁存，不自动放行 */
 assert(mp_calibration_shutdown()==0&&!mp_calibration_is_ready());
 return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.c';p.write_text(pre+code+test)
 subprocess.run(['gcc','-std=c99','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(src),str(p),'-o',d+'/test'],check=True)
 subprocess.run([d+'/test'],check=True)
print('actual MP initialization and RX-state gate tests PASS')
