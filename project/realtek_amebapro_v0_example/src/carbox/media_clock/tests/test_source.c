#include "AirPlayMediaClockSource.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    struct media_source s;struct media_sample e;MediaSource_Init(&s);
    unsigned g=MediaSource_Begin(&s,0x1234);assert(g && s.owner==0x1234);
    assert(MediaSource_Begin(&s,0x5678)==0);assert(s.conflicts==1);
    for(unsigned i=0;i<40;i++) {
        e=(struct media_sample){.generation=g,.local_us=(uint64_t)i*2000000,.source_us=(uint64_t)i*2000100,.flags=MEDIA_SYNC_VALID};
        MediaSource_Push(&s,0x1234,&e);
    }
    assert(s.lost>0);MediaSource_End(&s,0x1234);assert(!s.owner);
    unsigned g2=MediaSource_Begin(&s,0x1234);assert(g2!=g);
    MediaSource_Push(&s,0x1234,&e);assert(s.stale>0);
    while(MediaSource_Pop(&s,&e))assert(e.generation==g2);
    e=(struct media_sample){.generation=g2,.local_us=90000000,.source_us=90004500,.flags=MEDIA_SYNC_VALID};
    MediaSource_Push(&s,0x1234,&e);assert(MediaSource_Pop(&s,&e));
    assert(e.generation==g2 && (e.flags&MEDIA_SOURCE_RESET));
    puts("source owner, generation reuse, overflow invalidation, stale callback, reset PASS");
}
