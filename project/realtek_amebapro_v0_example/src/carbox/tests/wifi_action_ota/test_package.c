#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "carplay_ota_compat.c"
static uint8_t flashmem[8*1024*1024], package[56+256+128];
static unsigned package_size, supplied, declared, commits, writes, allocations;
void *pvPortMalloc(size_t n) {void *p=malloc(n);if(p)allocations++;return p;}
void vPortFree(void *p) {if(p){allocations--;free(p);}}
void device_mutex_lock(RT_DEV_LOCK_E d) {(void)d;}
void device_mutex_unlock(RT_DEV_LOCK_E d) {(void)d;}
void flash_erase_sector(flash_t *f,uint32_t a) {(void)f;assert(a+4096<=sizeof(flashmem));memset(flashmem+a,255,4096);}
int flash_burst_write(flash_t *f,uint32_t a,uint32_t n,uint8_t *b) {(void)f;assert(a+n<=sizeof(flashmem));memcpy(flashmem+a,b,n);writes++;return 1;}
int flash_stream_read(flash_t *f,uint32_t a,uint32_t n,uint8_t *b) {(void)f;assert(a+n<=sizeof(flashmem));memcpy(b,flashmem+a,n);return 1;}
uint32_t update_ota_prepare_addr(void) {return 0x3c0000;}
uint32_t update_ota_get_curr_fw_start_offset(void) {return 0x40000;}
int update_ota_signature(unsigned char *s,uint32_t a) {assert(a==0x3c0000);memcpy(flashmem+a,s,32);commits++;return 0;}
int carbox_ota_http_get(const char *h,int port,const char *p,
 int (*begin)(void *,uint32_t),int (*data)(void *,const uint8_t *,uint32_t),void *ctx) {
 unsigned pos=0;
 assert(h && port==8080 && p);
 if(begin(ctx,declared))return -1;
 while(pos<supplied) {unsigned n=supplied-pos;if(n>17)n=17;if(data(ctx,package+pos,n))return -1;pos+=n;}
 return supplied==declared?0:-1;
}
static void prepare(int two) {
 carplay_smart_file_hdr_t f={1,two?2:1};
 carplay_smart_img_hdr_t img={{'O','T','A',0},24,0,256,two?56:32,1};
 memset(flashmem,0,sizeof(flashmem));memset(package,0,sizeof(package));
 memcpy(package,&f,sizeof(f));
 for(unsigned i=0;i<256;i++){package[img.Offset+i]=(uint8_t)(i+1);img.Checksum+=package[img.Offset+i];}
 memcpy(package+8,&img,24);
 package_size=img.Offset+img.ImgLen;
 if(two) {img.ImgID=2;img.ImgLen=128;img.Offset=package_size;img.Checksum=128*7;memset(package+img.Offset,7,128);memcpy(package+32,&img,24);package_size+=128;}
 supplied=declared=package_size;commits=writes=0;
}
static void run(int success) {
 carplay_ota_context_t ctx={0};
 assert(!allocations);
 assert(ota_update_init(&ctx,"127.0.0.1",8080,"/ota_all.bin",1)==0);
 assert((ota_update_start(&ctx)==0)==success);
 assert(commits==(unsigned)success);
 assert(ota_update_deinit(&ctx)==0); assert(commits==(unsigned)success);
 assert(!allocations && !ctx.otactrl);
}
int main(void) {
 prepare(0);run(1);assert(!memcmp(flashmem+0x3c0000,package+32,256));
 prepare(1);run(1);assert(!memcmp(flashmem+0x3c0000,package+56,256));assert(!memcmp(flashmem+CARBOX_FATFS_BASE,package+56+256,128));
 prepare(1);supplied--;run(0);assert(!commits);
 prepare(1);declared++;run(0);assert(!writes && !commits);
 prepare(1);package[60]^=1;run(0);assert(!commits);
 prepare(1);package[package_size-1]^=1;run(0);assert(!commits);
 puts("action OTA package: PASS");
}
