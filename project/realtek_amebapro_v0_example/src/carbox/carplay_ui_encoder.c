/* Closed x264 ABI 76 bridge. Archive SHA/layout are gated by the build patch.
 * One synchronous, single-thread encoder transaction owns every core allocation.
 * On OOM, abort at the allocator boundary: vendor partial-init cleanup is unsafe.
 */
#include "carplay_ui_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#ifndef CARBOX_UI_ENCODER_QEMU
#include "FreeRTOS.h"
#include "task.h"
#define OWNER() ((void *)xTaskGetCurrentTaskHandle())
#define ENTER() taskENTER_CRITICAL()
#define LEAVE() taskEXIT_CRITICAL()
#else
#define OWNER() ((void *)1)
#define ENTER() ((void)0)
#define LEAVE() ((void)0)
#endif
#if UINTPTR_MAX != 0xffffffffU
#error The closed x264/surface ABI requires 32-bit pointers
#endif
extern void x264_param_default(void *);
extern int x264_param_parse(void *, const char *, const char *);
extern void *x264_encoder_open_76(void *);
extern int x264_encoder_encode(void *, void **, int *, void *, void *);
extern void x264_encoder_close(void *);
extern void *x264_frame_new(void *, int);
extern void *__real_x264_frame_pop_unused(void *, int);
extern int __real_x264_frame_copy_picture(void *, void *, void *);
extern void CSurface_delete(void *);

typedef struct { void *raw; size_t size; } allocation;
static struct {
    void *owner;
    jmp_buf abort;
    allocation blocks[512];
    size_t live, peak, calls;
    uint8_t *image, *output, *base[3];
    void *output_raw;
    size_t output_size;
    carbox_ui_layout layout;
    int borrowing;
    unsigned borrowed;
} ui
#ifndef CARBOX_UI_ENCODER_QEMU
    __attribute__((section(".lpddr.bss.carplay_ui_encoder")))
#endif
    ;
static int owned(void) { return ui.owner && ui.owner == OWNER(); }
static void heap_snapshot(const char *stage)
{
#ifndef CARBOX_UI_ENCODER_QEMU
    extern void vPortGetHeapFragmentationStatsExt(int, size_t *, size_t *, size_t *);
    size_t available=0, largest=0, blocks=0;
    vPortGetHeapFragmentationStatsExt(0,&available,&largest,&blocks);
    printf("[UI MEM] %s heap0_free=%u largest=%u blocks=%u\r\n",stage,
           (unsigned)available,(unsigned)largest,(unsigned)blocks);
#else
    (void)stage;
#endif
}
static void fail(const char *reason, size_t bytes)
{
    printf("[UI MEM] abort %s bytes=%u core_live=%u peak=%u\r\n",
           reason, (unsigned)bytes, (unsigned)ui.live, (unsigned)ui.peak);
    heap_snapshot("abort");
    longjmp(ui.abort, 1);
}
/* Renamed raw allocator references in common.o/encoder.o/x264.o also capture
 * strdup and the vendor's leaked NAL/CQM allocations. Other tasks pass through. */
void *carbox_ui_core_malloc(size_t n)
{
    void *p;
    if (!owned()) return malloc(n);
    unsigned slot;
    for (slot=0; slot<512 && ui.blocks[slot].raw; ++slot) {}
    if (slot==512) fail("allocation table", n);
    p=malloc(n);
    if (!p) fail("OOM", n);
    ui.blocks[slot].raw=p; ui.blocks[slot].size=n;
    ui.live+=n; ++ui.calls;
    if (ui.live>ui.peak) ui.peak=ui.live;
    return p;
}
void carbox_ui_core_free(void *p)
{
    if (!p) return;
    if (owned() && p==ui.output_raw) return;
    if (owned()) {
        for (unsigned i=0; i<512; ++i) if (ui.blocks[i].raw==p) {
            ui.live-=ui.blocks[i].size; ui.blocks[i].raw=NULL; break;
        }
    }
    free(p);
}
char *carbox_ui_core_strdup(const char *s)
{
    size_t n=strlen(s)+1; char *p=carbox_ui_core_malloc(n);
    if (p) memcpy(p,s,n);
    return p;
}
/* Strong definitions override weakened vendor definitions, including calls
 * originating in common.o itself. Preserve the vendor alignment/header ABI. */
void *x264_malloc(int n)
{
    uintptr_t caller=(uintptr_t)__builtin_return_address(0) & ~(uintptr_t)1;
    uintptr_t frame=(uintptr_t)x264_frame_new & ~(uintptr_t)1;
    if (owned() && ui.borrowing) {
        int plane=caller==frame+0xcc ? 1 : caller==frame+0xee ? 2 :
                  caller==frame+0x256 ? 0 : -1;
        if (plane>=0) {
            if (n != (int)ui.layout.bytes[plane] || (ui.borrowed & (1U<<plane)))
                fail("fenc ABI mismatch", n);
            ui.borrowed |= 1U<<plane;
            return ui.base[plane];
        }
    }
    if (n<0) { if (owned()) fail("invalid allocation", (size_t)n); return NULL; }
    void *raw=carbox_ui_core_malloc((size_t)n+23);
    if (!raw) return NULL;
    uint32_t *p=(uint32_t *)(((uintptr_t)raw+23)&~(uintptr_t)15);
    p[-1]=(uintptr_t)raw; p[-2]=(uint32_t)n;
    return p;
}
void x264_free(void *p)
{
    if (!p) return;
    if (owned() && p==ui.output) return;
    if (owned() && (p==ui.base[0] || p==ui.base[1] || p==ui.base[2])) return;
    carbox_ui_core_free((void *)(uintptr_t)((uint32_t *)p)[-1]);
}
void *__wrap_x264_frame_pop_unused(void *h, int fdec)
{
    if (!owned() || fdec) return __real_x264_frame_pop_unused(h,fdec);
    if (ui.borrowed) fail("second fenc unexpected", 0);
    ui.borrowing=1;
    void *f=__real_x264_frame_pop_unused(h,0);
    ui.borrowing=0;
    if (ui.borrowed!=7) fail("fenc allocations not intercepted", ui.borrowed);
    for (unsigned p=0; p<3; ++p)
        if (((uint32_t *)f)[(104+4*p)/4] != (uintptr_t)(ui.image+ui.layout.offset[p]))
            fail("fenc plane mismatch", p);
    printf("[UI MEM] fenc shares RGB allocation, padded=%u\r\n",(unsigned)ui.layout.total);
    return f;
}
int __wrap_x264_frame_copy_picture(void *h, void *f, void *pic)
{
    if (!owned()) return __real_x264_frame_copy_picture(h,f,pic);
    uint32_t *dst=f, *src=pic;
    for (unsigned p=0; p<3; ++p)
        if (dst[(104+4*p)/4]!=src[(44+4*p)/4] || dst[(56+4*p)/4]!=src[(28+4*p)/4])
            fail("picture alias mismatch",p);
    dst[1]=src[0]; dst[2]=src[1]; dst[4]=src[2]; dst[5]=src[3]; dst[6]=src[4];
    return 0; /* Metadata only; calling plane_copy with aliased planes is unsafe. */
}
static void option(void *p, const char *key, const char *value)
{
    if (x264_param_parse(p,key,value)) fail(key,0);
}
static void release_core(void)
{
    size_t remaining=ui.live;
    for (unsigned i=0; i<512; ++i) if (ui.blocks[i].raw) {
        free(ui.blocks[i].raw); ui.blocks[i].raw=NULL;
    }
    ui.live=0;
    printf("[UI MEM] core released leftovers=%u peak=%u allocations=%u\r\n",
           (unsigned)remaining,(unsigned)ui.peak,(unsigned)ui.calls);
}
/* Called by the audited Accessory.o tail shim after all UI drawing is done.
 * This function consumes/deletes the surface on every path. */
void carbox_ui_encode_surface(void *surface, int width, int height, int fps,
                              const char *filename)
{
    uint32_t param[148], pic[16]={0}, outpic[16]={0};
    carbox_ui_skip skip={0};
    uint32_t *s=surface;
    ENTER();
    if (ui.owner) { LEAVE(); CSurface_delete(surface); printf("[UI MEM] busy\r\n"); return; }
    ui.owner=OWNER(); LEAVE();
    if (setjmp(ui.abort)) goto cleanup;
    if (!s) goto cleanup;
    int w=s[0], h=s[1], bpp=s[4];
    if (width<=0 || height<=0 || width>4096 || height>4096 ||
        (width&1) || (height&1) || w!=((width+15)&~15) || h!=((height+15)&~15) ||
        (bpp!=2 && bpp!=3) || s[3]!=(uint32_t)(w*bpp) || !s[6] ||
        carbox_ui_layout_init(&ui.layout,w,h)) {
        CSurface_delete(surface); surface=NULL; fail("surface layout",0);
    }
    ui.image=(uint8_t *)(uintptr_t)s[6]; s[6]=0;
    CSurface_delete(surface); surface=NULL;
    printf("[UI MEM] start %dx%d surface=%dx%d bpp=%d rgb=%u scratch=%u\r\n",
           width,height,w,h,bpp,(unsigned)(w*h*bpp),(unsigned)(2*w*bpp+(h*3+7)/8));
    if (carbox_ui_inplace(ui.image,bpp,w,h)) fail("conversion scratch",0);
    /* At small resolutions x264 padding can exceed the RGB allocation. */
    if (ui.layout.total>(size_t)w*h*bpp) {
        uint8_t *p=malloc(ui.layout.total);
        if (!p) fail("small-frame padding",ui.layout.total);
        memcpy(p,ui.image,(size_t)w*h*3/2); free(ui.image); ui.image=p;
    }
    carbox_ui_pad(ui.image,&ui.layout,w,h);
    ui.base[0]=ui.image; ui.base[1]=ui.image+ui.layout.bytes[0];
    ui.base[2]=ui.base[1]+ui.layout.bytes[1];
    x264_param_default(param);
#define P(offset,value) param[(offset)/4]=(value)
    P(0,0); P(4,1); P(16,width); P(20,height); P(24,1); P(32,0);
    P(48,5); P(52,1); P(56,1); P(60,1); P(64,6);
    P(72,fps>0 && fps<=120 ? fps : 20); P(76,1); P(80,1); P(84,1024);
    P(96,0); P(396,0); P(424,0);
    option(param,"qp","23"); option(param,"scenecut","0");
    option(param,"b-adapt","0"); option(param,"mbtree","0");
    option(param,"rc-lookahead","0"); option(param,"ipratio","1");
    option(param,"pbratio","1"); option(param,"cabac","0");
    heap_snapshot("before_open");
    void *encoder=x264_encoder_open_76(param);
    if (!encoder) fail("encoder open",0);
    pic[0]=1; /* Explicit IDR; no future input/reference pixels will be encoded. */
    pic[5]=1; pic[6]=3;
    for (unsigned p=0;p<3;++p) {
        pic[7+p]=ui.layout.stride[p]; pic[11+p]=(uintptr_t)(ui.image+ui.layout.offset[p]);
    }
    void *nals=NULL; int count=0;
    int bytes=x264_encoder_encode(encoder,&nals,&count,pic,outpic);
    if (bytes<=0 || !nals || count<=0 || count>32) fail("IDR encode",0);
    heap_snapshot("after_IDR");
    size_t total=0; int have_idr=0;
    for (int i=0;i<count;++i) {
        uint32_t *n=(uint32_t *)nals+4*i;
        if (n[2]>16*1024*1024 || total>16*1024*1024-n[2] ||
            carbox_ui_read_headers(&skip,(uint8_t *)(uintptr_t)n[3],n[2])) fail("NAL/header",i);
        total+=n[2]; if (n[1]==5) have_idr=1;
    }
    if (!have_idr || !skip.have_pps || skip.width!=(unsigned)width || skip.height!=(unsigned)height)
        fail("unsupported SPS/PPS",0);
    /* NALs occupy one x264-owned allocation. Transfer that allocation instead
     * of copying the IDR while fenc/fdec are still live. Verify every boundary. */
    uint8_t *payload=(uint8_t *)(uintptr_t)((uint32_t *)nals)[3];
    size_t at=0;
    for (int i=0;i<count;++i) {
        uint32_t *n=(uint32_t *)nals+4*i;
        if ((uintptr_t)payload+at!=n[3]) fail("noncontiguous NALs",i);
        at+=n[2];
    }
    for (unsigned i=0;i<512;++i) if (ui.blocks[i].raw) {
        uintptr_t raw=(uintptr_t)ui.blocks[i].raw, addr=(uintptr_t)payload;
        if (addr>=raw && addr-raw<=23 && addr-raw<=ui.blocks[i].size && total<=ui.blocks[i].size-(addr-raw)) {
            ui.output_raw=ui.blocks[i].raw; ui.output=payload; ui.output_size=total;
            ui.live-=ui.blocks[i].size; ui.blocks[i].raw=NULL;
            break;
        }
    }
    if (!ui.output_raw) fail("NAL allocation ownership",total);
    x264_encoder_close(encoder);
    release_core();
    free(ui.image); ui.image=NULL; memset(ui.base,0,sizeof(ui.base));
    heap_snapshot("after_encoder_release");
    printf("[UI MEM] IDR=%u; encoder and pixels freed before skip20\r\n",(unsigned)total);
    FILE *file=fopen(filename,"wb");
    if (file) {
        uint8_t packet[128];
        size_t written=fwrite(ui.output,1,total,file), skip_bytes=0;
        int ok=written==total;
        for (unsigned frame=1;ok && frame<=20;++frame) {
            size_t n=carbox_ui_skip_frame(&skip,frame,packet,sizeof(packet));
            if (!n) { ok=0; break; }
            size_t done=fwrite(packet,1,n,file);
            written+=done; skip_bytes+=done;
            if (done!=n) ok=0;
        }
        int closed=fclose(file);
        printf("[UI MEM] saved %s bytes=%u skip=%u status=%s\r\n",filename,
               (unsigned)written,(unsigned)skip_bytes,ok && !closed ? "OK" : "WRITE_FAILED");
    } else printf("[UI MEM] file open failed: %s\r\n",filename);
cleanup:
    release_core();
    free(ui.image); free(ui.output_raw);
    /* Do not memset jmp_buf/owner until there are no more core calls. */
    ui.image=NULL; ui.output=NULL; ui.output_raw=NULL; ui.output_size=0;
    memset(ui.base,0,sizeof(ui.base)); ui.borrowing=0; ui.borrowed=0;
    ui.peak=ui.calls=0;
    heap_snapshot("done");
    ENTER(); ui.owner=NULL; LEAVE();
}
