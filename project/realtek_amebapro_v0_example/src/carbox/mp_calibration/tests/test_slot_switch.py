"""执行两个产品命令实现，检查缓冲失败、FW2 缺失及 flash 锁顺序。"""
from pathlib import Path
import subprocess
import tempfile

root = next(p for p in Path(__file__).resolve().parents if (p / 'component').is_dir())
paths = [root / 'component/common/api/at_cmd/atcmd_sys.c',
         Path(__file__).resolve().parents[1] / 'recovery_atcmd_sys.c']
pre = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
typedef int flash_t;
typedef struct {int fw1_valid,fw2_valid,loaded_fw_idx;uint32_t fw1_start_offset;} fw_img_export_info_type_t;
static fw_img_export_info_type_t info;
static int fail_alloc, locked, critical, resets, writes;
static unsigned char sector[4096];
static void *get_fw_img_info_tbl(void){return &info;}
static void *test_malloc(size_t n){return fail_alloc?NULL:malloc(n);}
#define malloc test_malloc
static void print_stub(const char *fmt,...){(void)fmt;}
#define AT_PRINTK print_stub
#define RT_DEV_LOCK_FLASH 0
static void device_mutex_lock(int x){(void)x;assert(!critical&&!locked);locked=1;}
static void device_mutex_unlock(int x){(void)x;assert(!critical&&locked);locked=0;}
static void rtw_enter_critical(void *a,void *b){(void)a;(void)b;assert(locked&&!critical);critical=1;}
static void rtw_exit_critical(void *a,void *b){(void)a;(void)b;assert(locked&&critical);critical=0;}
static int flash_stream_read(flash_t *f,uint32_t a,uint32_t n,uint8_t *p){(void)f;(void)a;assert(locked&&critical);memcpy(p,sector,n);return 1;}
static void flash_erase_sector(flash_t *f,uint32_t a){(void)f;(void)a;assert(locked&&critical);memset(sector,255,sizeof(sector));}
static int flash_burst_write(flash_t *f,uint32_t a,uint32_t n,uint8_t *p){(void)f;(void)a;assert(locked&&critical);memcpy(sector,p,n);writes++;return 1;}
static void sys_reset(void){assert(!locked&&!critical);resets++;}
'''
test = r'''
int main(void){
 info=(fw_img_export_info_type_t){1,0,1,0x40000};
 fOTARECOVER(NULL);assert(!writes&&!resets);
 info.fw2_valid=1;fail_alloc=1;
 fOTARECOVER(NULL);assert(!writes&&!resets);
 info.fw1_valid=0;fOTACLEAR(NULL);assert(!writes&&!resets);
 fail_alloc=0;info.fw1_valid=1;sector[0]=0x38;
 fOTARECOVER(NULL);assert(writes==1&&resets==1&&sector[0]==(unsigned char)~0x38);
 info.fw1_valid=0;info.loaded_fw_idx=2;
 fOTARECOVER(NULL);assert(writes==1&&resets==1);
 fOTACLEAR(NULL);assert(writes==2&&resets==2&&sector[0]==0x38);
 return 0;
}
'''
for source in paths:
    s = source.read_text()
    start = s.index('#define CARBOX_OTA_SECTOR_SIZE')
    stop = s.index('\n}', s.index('void fOTACLEAR', start)) + 2
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / 'test.c'
        p.write_text(pre + s[start:stop] + test)
        subprocess.run(['gcc', '-std=c99', '-Wall', '-Wextra', '-Werror', str(p), '-o', d + '/test'], check=True)
        subprocess.run([d + '/test'], check=True)
print('Normal/Recovery slot-switch failure and lock-order tests PASS')
