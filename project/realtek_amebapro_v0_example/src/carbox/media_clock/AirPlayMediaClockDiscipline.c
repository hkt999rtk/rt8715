#include "AirPlayMediaClockDiscipline.h"
#include <string.h>
#define SCALE 1000000000LL
#define LIMIT_PPB 200000LL
#define STALE_US 10000000ULL
static uint64_t mapped(const struct media_discipline *s,uint64_t now,uint64_t *fraction)
{
    uint64_t d=now>s->anchor_local?now-s->anchor_local:0;
    /* 拆开乘法，避免长时间 uptime * ppb 溢出；负数按向下取整，保留余数。 */
    int64_t q=(int64_t)(d/SCALE)*s->applied_ppb;
    int64_t r=(int64_t)(d%SCALE)*s->applied_ppb+(int64_t)s->fraction;
    q+=r/SCALE;r%=SCALE;if(r<0){r+=SCALE;--q;}
    if(fraction)*fraction=(uint64_t)r;
    return s->anchor_media+d+q;
}
uint64_t MediaDiscipline_Now(const struct media_discipline *s,uint64_t now) { return mapped(s,now,0); }
void MediaDiscipline_Init(struct media_discipline *s,uint64_t local,uint64_t media)
{
    memset(s,0,sizeof(*s));s->anchor_local=local;s->anchor_media=media;s->last_update=local;
}
void MediaDiscipline_SourceReset(struct media_discipline *s,uint32_t generation)
{
    s->generation=generation;s->count=0;s->have_sync=0;
    s->state=s->applied_ppb?MEDIA_HOLDOVER:MEDIA_LOCAL;
    s->target_ppb=s->applied_ppb;
}
static void hold(struct media_discipline *s) {
    s->state=MEDIA_HOLDOVER;s->count=0;s->target_ppb=s->applied_ppb;
}
void MediaDiscipline_Observe(struct media_discipline *s,uint32_t gen,uint64_t l,uint64_t source,uint64_t sync,uint32_t flags)
{
    unsigned i,n;int64_t sx=0,sy=0,sxx=0,sxy=0,cov,den,ppb;
    if(gen!=s->generation){s->rejected++;return;}
    if(!(flags&MEDIA_SYNC_VALID)||sync>l||l-sync>STALE_US||
       flags&(MEDIA_PHASE_STEP|MEDIA_SOURCE_RESET|MEDIA_SOURCE_SETTLING)) {
        s->rejected++;s->steps+=!!(flags&MEDIA_PHASE_STEP);hold(s);return;
    }
    if(s->have_sync && (sync<s->last_sync || l<s->last_sync)) {s->rejected++;return;}
    if(s->have_sync && sync-s->last_sync>STALE_US)hold(s);
    s->last_sync=sync;s->have_sync=1;
    n=s->count;
    if(n) {
        uint64_t dl=l-s->local[n-1],ds=source-s->source[n-1];
        if(l<=s->local[n-1]||source<=s->source[n-1]||dl>STALE_US||
           (ds>dl?ds-dl:dl-ds)>5000+dl/100) {s->rejected++;hold(s);return;}
        if(dl<1000000) return; /* 接受同步的新鲜度，但不把密集协商回复当作独立窗口。 */
    }
    while(s->count && (s->count==MEDIA_WINDOW_POINTS || l-s->local[0]>60000000)) {
        --s->count;
        memmove(s->local,s->local+1,s->count*sizeof(s->local[0]));
        memmove(s->source,s->source+1,s->count*sizeof(s->source[0]));
    }
    n=s->count;s->local[n]=l;s->source[n]=source;s->count=++n;
    if(n<10 || l-s->local[0]<30000000){s->state=MEDIA_ACQUIRE;return;}
    /* 拟合 residual=(source_delta-local_delta)，x 用 ms 限制平方量级。
     * 不使用 source 的巨大绝对值；整数商/余数分步缩放防止乘法溢出。 */
    for(i=0;i<n;i++) {
        int64_t dl=(int64_t)(s->local[i]-s->local[0]);
        int64_t x=dl/1000,y=(int64_t)(s->source[i]-s->source[0])-dl;
        sx+=x;sy+=y;sxx+=x*x;sxy+=x*y;
    }
    cov=(int64_t)n*sxy-sx*sy;den=(int64_t)n*sxx-sx*sx;
    if(den<=0){s->rejected++;hold(s);return;}
    ppb=(cov/den)*1000000+(cov%den)*1000000/den;
    s->estimated_ppb=ppb;
    if(ppb>LIMIT_PPB||ppb< -LIMIT_PPB){s->out_of_range++;hold(s);return;}
    s->residual_us=0;
    for(i=0;i<n;i++) {
        int64_t dl=(int64_t)(s->local[i]-s->local[0]);
        int64_t y=(int64_t)(s->source[i]-s->source[0])-dl;
        int64_t predicted=sy/(int64_t)n+((dl/1000)*(int64_t)n-sx)*ppb/((int64_t)n*1000000);
        int64_t e=y-predicted;if(e<0)e=-e;
        if(e>s->residual_us)s->residual_us=(uint32_t)e;
    }
    if(s->residual_us>2000){s->rejected++;hold(s);return;}
    s->target_ppb=ppb;s->state=MEDIA_TRACK;
}
void MediaDiscipline_Update(struct media_discipline *s,uint64_t now,int apply)
{
    uint64_t elapsed;int64_t limit,change;
    if(now<s->last_update)return;
    elapsed=now-s->last_update;s->last_update=now;
    if(s->have_sync && now>=s->last_sync && now-s->last_sync>STALE_US)hold(s);
    if(!apply||s->state!=MEDIA_TRACK)return;
    /* 2 ppm / 实际秒；先用旧速率锚定，保留不足 1us 的相位余量。 */
    s->anchor_media=mapped(s,now,&s->fraction);s->anchor_local=now;
    limit=elapsed>=200000000?400000:(int64_t)(elapsed*2000/1000000);
    change=s->target_ppb-s->applied_ppb;
    if(change>limit)change=limit;
    if(change< -limit)change= -limit;
    s->applied_ppb+=change;
}
uint64_t MediaDiscipline_LocalDeadline(const struct media_discipline *s,uint64_t media)
{
    uint64_t lo=s->anchor_local,hi,d;
    if(media<=s->anchor_media)return lo;
    d=media-s->anchor_media;
    /* 约定期限在当前锚点之后；上界覆盖 +/-200ppm，用二分实现精确向上取整。 */
    if(d>UINT64_MAX-lo-d/1000-2)return UINT64_MAX;
    hi=lo+d+d/1000+2;
    while(lo<hi){uint64_t mid=lo+(hi-lo)/2;if(MediaDiscipline_Now(s,mid)<media)lo=mid+1;else hi=mid;}
    return lo;
}
