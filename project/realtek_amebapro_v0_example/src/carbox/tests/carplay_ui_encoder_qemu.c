/* Host-only QEMU ARM harness for the exact derived vendor archive. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <errno.h>
#include <stddef.h>
static long sys3(long n,long a,long b,long c){register long r0 asm("r0")=a;register long r1 asm("r1")=b;register long r2 asm("r2")=c;register long r7 asm("r7")=n;asm volatile("svc 0":"+r"(r0):"r"(r1),"r"(r2),"r"(r7):"memory");return r0;}
void _exit(int n){sys3(1,n,0,0);for(;;);}
int _write(int fd,const void *p,int n){return sys3(4,fd,(long)p,n);}
int _read(int fd,void *p,int n){return sys3(3,fd,(long)p,n);}
int _close(int fd){return sys3(6,fd,0,0);}
int _isatty(int fd){return fd<=2;}
int _fstat(int fd,struct stat *s){memset(s,0,sizeof(*s));s->st_mode=S_IFCHR;return 0;}
int _lseek(int fd,int off,int whence){return sys3(19,fd,off,whence);}
int _gettimeofday(struct timeval *t,void *tz){return sys3(78,(long)t,(long)tz,0);}
int _open(const char *p,int flags,int mode){return sys3(5,(long)p,577,mode);}
int _kill(int a,int b){return -1;} int _getpid(void){return 1;}
int _unlink(const char *p){return -1;}
static char arena[96*1024*1024] __attribute__((aligned(16)));static ptrdiff_t arena_used;
void *_sbrk(ptrdiff_t n){if(arena_used+n<0 || arena_used+n>sizeof(arena)){errno=ENOMEM;return (void*)-1;}void *p=arena+arena_used;arena_used+=n;return p;}
void *__real_malloc(size_t);void __real_free(void*);

static size_t live, peak, calls, fail_at;
static unsigned guards;
typedef struct {void *raw; size_t n; uint32_t magic, reserved;} hdr;
void *__wrap_malloc(size_t n) {
    ++calls;
    if (fail_at && calls==fail_at) return NULL;
    void *raw=__real_malloc(n+sizeof(hdr)+15+64);
    if (!raw) return NULL;
    hdr *h=(hdr *)(((uintptr_t)raw+sizeof(hdr)+15)&~(uintptr_t)15)-1;
    h->raw=raw; h->n=n; h->magic=0x26326326;
    memset((uint8_t *)(h+1)+n,0xa5,64);
    live+=n; if(live>peak) peak=live;
    return h+1;
}
void __wrap_free(void *p) {
    if(!p)return;
    hdr *h=(hdr *)p-1;
    if(h->magic!=0x26326326){printf("BADFREE %p\n",p);_exit(2);}
    for(int j=0;j<64;j++)if(((uint8_t *)p)[h->n+j]!=0xa5) {
        printf("GUARDFAIL bytes=%u offset=%d\n",(unsigned)h->n,j); ++guards; break;
    }
    live-=h->n; h->magic=0; __real_free(h->raw);
}
void *__wrap_realloc(void *p,size_t n) {
    if(!p)return __wrap_malloc(n);
    void *q=__wrap_malloc(n); if(q){size_t old=((hdr *)p-1)->n;memcpy(q,p,n<old?n:old);__wrap_free(p);}return q;
}
void *__wrap_calloc(size_t a,size_t b){void *p=__wrap_malloc(a*b);if(p)memset(p,0,a*b);return p;}
char *__wrap_strdup(const char *s){size_t n=strlen(s)+1;char *p=__wrap_malloc(n);if(p)memcpy(p,s,n);return p;}
int _link(const char *a,const char *b){return -1;}
void CSurface_delete(void *surface){uint32_t *s=surface;if(s){__wrap_free((void *)s[6]);__wrap_free((void *)s[9]);__wrap_free(s);}}
extern void carbox_ui_encode_surface(void *,int,int,int,const char *);
int main(int argc,char **argv){
    int width=argc>1?atoi(argv[1]):2560,height=argc>2?atoi(argv[2]):1440;
    int w=(width+15)&~15,h=(height+15)&~15;
    size_t failure=argc>3?atoi(argv[3]):0;
    unsigned random=1234567;
    for(int repeat=0;repeat<2;++repeat){
        fail_at=0;
        uint32_t *surface=__wrap_calloc(10,4);uint16_t *rgb=__wrap_malloc(w*h*2);
        for(int y=0;y<h;++y)for(int x=0;x<w;++x){
            random^=random<<13;random^=random>>17;random^=random<<5;
            rgb[y*w+x]=argc>4 && !strcmp(argv[4],"random") ? random : ((x/16+y/16)&1)?0xffff:0;
        }
        surface[0]=w;surface[1]=h;surface[2]=2;surface[3]=w*2;surface[4]=2;surface[6]=(uintptr_t)rgb;
        fail_at=failure?calls+failure:0;
        carbox_ui_encode_surface(surface,width,height,20,argc>5?argv[5]:"/tmp/rt8715-x264-review/integrated.h264");
        printf("RESULT repeat=%d live=%u peak=%u calls=%u guards=%u\n",repeat,(unsigned)live,(unsigned)peak,(unsigned)calls,guards);
        if(live || guards)return 2;
    }
    return 0;
}
