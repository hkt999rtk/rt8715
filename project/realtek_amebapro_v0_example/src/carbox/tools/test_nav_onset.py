#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
test=r'''
#include "audio_nav_pacing.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
int main(void){
 struct nav_pacer p;uint32_t first=UINT32_MAX-10000U;
 nav_pace_init(&p,192000,100000,first-20000U);
 nav_pace_pop(&p,first-10000U,6240,1440,1);
 nav_pace_send_enter(&p,first,1,0);nav_pace_send_exit(&p,first+40000U);
 assert(p.first_q_us==32500 && p.onset_send_max==40000);
 nav_pace_observe(&p,20000);p.boost_ppm=3000;p.debt_us=35000;
 nav_pace_send_enter(&p,first+45000U,2,360);nav_pace_send_exit(&p,first+45100U);
 nav_pace_send_enter(&p,first+50000U,3,720);nav_pace_send_exit(&p,first+50100U);
 assert(p.onset_gap_max==45000 && p.onset_gap_min==5000 && p.onset_gap_total==50000);
 assert(p.onset_fast_n==1 && p.onset_slow_n==1 && p.onset_q_max==104166);
 nav_pace_pop(&p,first+100000U,0,1440,0);
 nav_pace_pop(&p,first+110000U,6240,1440,1);
 assert(p.onset_empty_n==1 && p.onset_boost_max==3000 && p.onset_debt_max==35000);
 nav_pace_send_enter(&p,first+1000000U,4,1080);nav_pace_send_exit(&p,first+1200000U);
 assert(p.onset_n==3 && p.onset_send_max==40000 && p.first_us==first);
 nav_pace_send_enter(&p,first+500U,5,1440); /* Next full HAL wrap must not reopen. */
 assert(p.onset_n==3 && p.onset_closed);
 puts("PASS first-second evidence: wrap, boundary, queue, stall, burst, resume");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'t.c').write_text(test)
 subprocess.run(['cc','-Wall','-Wextra','-Werror','-fsanitize=undefined','-I',str(root),str(p/'t.c'),str(root/'audio_nav_pacing.c'),'-o',str(p/'t')],check=True)
 subprocess.run([str(p/'t')],check=True)
