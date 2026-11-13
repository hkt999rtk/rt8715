#include "AirPlayMediaClockDiscipline.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static void feed(struct media_discipline *s, uint64_t t, int ppm, int active) {
    uint64_t src=900000000000ULL+t+(int64_t)t*ppm/1000000;
    MediaDiscipline_Observe(s,7,t,src,t,MEDIA_SYNC_VALID);
    MediaDiscipline_Update(s,t,active);
}
int main(void) {
    struct media_discipline s;
    int rates[]={50,-50,200,-200}; unsigned j;
    for(j=0;j<4;j++) {
        int ppm=rates[j]; uint64_t t,prev=0;
        MediaDiscipline_Init(&s,0,0);MediaDiscipline_SourceReset(&s,7);
        for(t=0;t<=172800000000ULL;t+=2000000) {
            uint64_t before=MediaDiscipline_Now(&s,t),after,deadline;
            feed(&s,t,ppm,1);after=MediaDiscipline_Now(&s,t);
            assert(after==before);assert(after>=prev);prev=after;
            deadline=MediaDiscipline_LocalDeadline(&s,after+1000000);
            assert(MediaDiscipline_Now(&s,deadline)>=after+1000000);
            assert(MediaDiscipline_Now(&s,deadline-1)<after+1000000);
            if(t<30000000)assert(s.state!=MEDIA_TRACK);
        }
        assert(llabs(s.applied_ppb-ppm*1000)<20);
        assert(llabs(s.target_ppb-ppm*1000)<20);
        /* 失联冻结最后实际速率，重新取得完整窗口才可恢复。 */
        int64_t old=s.applied_ppb;MediaDiscipline_Update(&s,t+11000000,1);
        assert(s.state==MEDIA_HOLDOVER && s.applied_ppb==old);
        uint64_t before=MediaDiscipline_Now(&s,t+12000000);
        MediaDiscipline_SourceReset(&s,8);
        assert(MediaDiscipline_Now(&s,t+12000000)==before);
        MediaDiscipline_Observe(&s,7,t+12000000,0,t,MEDIA_SYNC_VALID);
        assert(s.count==0 && s.rejected);
    }
    MediaDiscipline_Init(&s,0,123);MediaDiscipline_SourceReset(&s,7);
    for(uint64_t t=0;t<=100000000;t+=2000000)feed(&s,t,50,0);
    assert(s.target_ppb==50000 && s.applied_ppb==0);
    assert(MediaDiscipline_Now(&s,100000001)==100000124);
    MediaDiscipline_Observe(&s,7,102000000,999,102000000,MEDIA_SYNC_VALID|MEDIA_PHASE_STEP);
    assert(s.state==MEDIA_HOLDOVER && s.count==0);
    MediaDiscipline_Init(&s,0,0);MediaDiscipline_SourceReset(&s,7);
    for(uint64_t t=0;t<=80000000;t+=2000000)feed(&s,t,500,1);
    assert(s.state!=MEDIA_TRACK && s.applied_ppb==0 && s.out_of_range);
    MediaDiscipline_Init(&s,0,0);MediaDiscipline_SourceReset(&s,7);
    for(uint64_t t=0;t<=30000000;t+=2000000)feed(&s,t,200,1);
    int64_t a=s.applied_ppb;MediaDiscipline_Update(&s,31000000,1);
    assert(s.applied_ppb-a<=2000);
    a=s.applied_ppb;MediaDiscipline_Update(&s,31500000,1);
    assert(s.applied_ppb-a<=1000);
    MediaDiscipline_Observe(&s,7,32000000,999,32000000,0);
    a=s.applied_ppb;MediaDiscipline_Update(&s,33000000,1);
    assert(s.state==MEDIA_HOLDOVER && s.applied_ppb==a);
    puts("48h +/-50/200ppm, deadline inverse, continuity, acquire, generation, phase-step, stale, shadow, outlier, elapsed slew PASS");
}
