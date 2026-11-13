#include "AirPlayMediaClockSource.h"
#include <string.h>
void MediaSource_Init(struct media_source *s){memset(s,0,sizeof(*s));}
uint32_t MediaSource_Begin(struct media_source *s,uintptr_t owner)
{
    if(!owner)return 0;
    if(s->owner==owner)return s->generation;
    if(s->owner){s->conflicts++;return 0;}
    s->owner=owner;if(!++s->generation)++s->generation;
    s->head=s->count=0;s->reset_pending=1;return s->generation;
}
void MediaSource_End(struct media_source *s,uintptr_t owner)
{
    if(s->owner!=owner)return;
    s->owner=0;s->head=s->count=0;s->reset_pending=1;
}
void MediaSource_Push(struct media_source *s,uintptr_t owner,const struct media_sample *e)
{
    if(!owner || s->owner!=owner || e->generation!=s->generation){s->stale++;return;}
    /* 队列丢样本后，不能跨越未知 phase-step 拟合：清窗并标记重新获取。 */
    if(s->count==MEDIA_SOURCE_QUEUE){s->lost+=s->count;s->head=s->count=0;s->reset_pending=1;}
    unsigned pos=(s->head+s->count)%MEDIA_SOURCE_QUEUE;
    s->queue[pos]=*e;
    if(s->reset_pending){s->queue[pos].flags|=MEDIA_SOURCE_RESET;s->reset_pending=0;}
    ++s->count;
}
int MediaSource_Pop(struct media_source *s,struct media_sample *out)
{
    if(!s->count)return 0;
    *out=s->queue[s->head];s->head=(s->head+1)%MEDIA_SOURCE_QUEUE;--s->count;return 1;
}
