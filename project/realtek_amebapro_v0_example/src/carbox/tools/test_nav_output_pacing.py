#!/usr/bin/env python3
"""Deterministic timing and lifetime tests of the production pacing controller."""
import subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
TEST=r'''
#include "audio_nav_pacing.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>

/* Synthetic recurrent stalls, not a replay: log15 has only aggregate timings.
 * A 40 ms producer, 1 ms RTOS wake quantization, and per-send build/service cost.
 * Exercise both payload sizes and a timer wrap for 60 seconds, then recovery. */
static void recurrent_stalls(uint32_t origin, uint32_t payload) {
 struct nav_pacer p; uint64_t t=0, nextpush=40000, until=90000000;
 uint32_t q=7680, maxq=q, drops=0, n=0, gapmin=UINT32_MAX, previous=0;
 uint64_t min_gap=((uint64_t)payload*1000000*2+192000*3-1)/(192000*3);
 nav_pace_init(&p,192000,100000,origin);
 while(t<until) {
  while(nextpush<=t) {if(q+7680<=100000) q+=7680; else ++drops; nextpush+=40000;}
  if(q< payload) {
   nav_pace_pop(&p,origin+(uint32_t)t,q,payload,0);t+=1000;continue;
  }
  q-=payload;nav_pace_pop(&p,origin+(uint32_t)t,q,payload,1);
  t+=100; /* packet preparation */
  if(n) {uint32_t gap=(uint32_t)t-previous;if(gap<gapmin)gapmin=gap;}
  previous=(uint32_t)t;
  nav_pace_send_enter(&p,origin+(uint32_t)t,n,n*(payload/4));
  uint32_t cost=200;
  if(t<60000000) {if(n%60==59) cost=38000;else if(n%12==11) cost=20000;}
  ++n;t+=cost;
  nav_pace_send_exit(&p,origin+(uint32_t)t);
  while(nextpush<=t) {if(q+7680<=100000) q+=7680;else ++drops;nextpush+=40000;}
  if(q>maxq)maxq=q;
  nav_pace_schedule(&p,origin+(uint32_t)t,q,payload);
  assert(p.min_gap_us==min_gap);
  int32_t wait=(int32_t)(p.deadline_us-(origin+(uint32_t)t));
  if(wait>0)t+=((uint32_t)wait+999)/1000*1000;
 }
 printf("recurrent stalls payload=%u: drops=%u max_queue_us=%u final_queue_us=%u min_gap_us=%u\n",
        payload,drops,(uint32_t)(maxq*1000000ULL/192000),(uint32_t)(q*1000000ULL/192000),gapmin);fflush(stdout);
 assert(drops==0);
 assert(maxq<70000); /* headroom for another producer batch */
 assert(q<19200); /* returns below 100 ms after stalls stop */
 assert(gapmin>=min_gap); /* no zero-wait packet burst */
}
int main(void) {
 recurrent_stalls(0,1440);
 recurrent_stalls(UINT32_MAX-100000,1416);
 /* A 164 ms stall must not cause an unbounded burst or a new full wait.
  * After the immediately due send, enforce a finite interval on every send.
  * Empty/resume cancels old timing debt. */
 {
  struct nav_pacer p;uint32_t t=UINT32_MAX-10000;
  nav_pace_init(&p,192000,100000,t);nav_pace_pop(&p,t,50000,1440,1);
  nav_pace_send_enter(&p,t,1,0);t+=164000;
  nav_pace_send_exit(&p,t);nav_pace_schedule(&p,t,50000,1440);
  assert(p.deadline_us==t && p.debt_us==40000 && p.deadline_rebases==1);
  for(int i=0;i<30;i++) {
   nav_pace_send_enter(&p,t,i+2,360*(i+1));
   nav_pace_send_exit(&p,t+200);nav_pace_schedule(&p,t+200,50000,1440);
   assert(p.deadline_us-t>=5000 && p.deadline_us-t<=7500);
   t=p.deadline_us;
  }
  nav_pace_pop(&p,t,0,1440,0);t+=100000;
  nav_pace_pop(&p,t,6240,1440,1);
  assert(p.phase_us==t && p.debt_us==0 && p.boost_ppm==0);
  nav_pace_send_enter(&p,t,99,999);nav_pace_schedule(&p,t+200,6240,1440);
  assert(p.deadline_us-t==7500);
 }


 struct nav_pacer p; uint32_t now=100,prev;
 nav_pace_init(&p,192000,100000,now);
 nav_pace_pop(&p,now,0,1440,0);
 nav_pace_pop(&p,now+20000,6240,1440,1);
 assert(p.startup_empty_us==20000 && p.empty_resume_n==0);
 now+=20000;
 for(int i=0;i<100;i++) {
  nav_pace_schedule(&p,now,6240,1440);
  assert(p.boost_ppm==0 && p.deadline_us-now==7500);
  now=p.deadline_us;
 }
 /* Sustained backlog reaches the 5% cap smoothly, never skips a packet wait. */
 prev=0;
 for(int i=0;i<200;i++) {
  nav_pace_schedule(&p,now,24000,1440);
  assert(p.boost_ppm<=50000 && p.boost_ppm>=prev);
  assert(p.boost_ppm-prev<=800);
  assert(p.deadline_us-now>=7142 && p.deadline_us-now<=7500);
  prev=p.boost_ppm;now=p.deadline_us;
 }
 assert(p.boost_ppm==50000);
 uint32_t begin=now;
 for(int i=0;i<700;i++){nav_pace_schedule(&p,now,24000,1440);now=p.deadline_us;}
 assert(now-begin>=4999999 && now-begin<=5000001); /* fractional 7,142.857 us */
 nav_pace_schedule(&p,now+42311,24000,1440);
 assert(p.late_max_us>35000 && p.debt_us<=40000);
 assert(p.deadline_us==now+42311); /* no additional interval after a stalled send */
 now=p.deadline_us;
 nav_pace_schedule(&p,now,1440,1440);assert(p.boost_ppm==0);
 /* Empty episodes count actual duration, not 1 ms polls; terminal stays separate. */
 nav_pace_pop(&p,10000000,0,1440,0);
 nav_pace_pop(&p,10001000,0,1440,0);
 nav_pace_pop(&p,10012000,1440,1440,1);
 assert(p.empty_resume_n==1 && p.empty_resume_max_us==12000);
 nav_pace_pop(&p,10020000,0,1440,0);nav_pace_finish(&p,10050000);
 assert(p.empty_resume_n==1 && p.terminal_empty_us==30000);
 nav_pace_init(&p,192000,100000,UINT32_MAX-4000);
 nav_pace_pop(&p,UINT32_MAX-4000,6240,1440,1);
 nav_pace_schedule(&p,UINT32_MAX-4000,6240,1440);
 assert(p.deadline_us==3499 && p.deadline_rebases==0);
 nav_pace_send_enter(&p,UINT32_MAX-3000,7,480);
 nav_pace_send_exit(&p,UINT32_MAX-2000);
 nav_pace_send_enter(&p,8000,8,840);nav_pace_send_exit(&p,9000);
 assert(p.send_gap_max_us==11001 && p.return_gap_max_us==11001);
 assert(p.gap_seq==8 && p.gap_ts==840 && p.gap_at_us==8000);
 assert(p.gap_gt10ms==1 && p.gap_gt20ms==0);
 /* 40 ms batch input: smoothing doesn't turn the rate fully on/off each batch. */
 nav_pace_init(&p,192000,100000,0);nav_pace_pop(&p,0,14400,1440,1);
 now=0;uint32_t q=14400,nextpush=40000,maxstep=0;
 for(int i=0;i<1200;i++) {
  while((int32_t)(now-nextpush)>=0){q+=7680;nextpush+=40000;}
  if(q>=1440)q-=1440;else q=0;
  prev=p.boost_ppm;nav_pace_schedule(&p,now,q,1440);
  uint32_t step=p.boost_ppm>prev?p.boost_ppm-prev:prev-p.boost_ppm;
  if(step>maxstep)maxstep=step;
  assert(q<30000 && p.boost_ppm<=50000);
  now=p.deadline_us;
 }
 assert(maxstep<1000);
 puts("PASS navigation pacing: gradual control, fractional timing, wrap, stall, batching, empty lifetime, send gaps");
}
'''
with tempfile.TemporaryDirectory(prefix='nav-output-pacing-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(TEST)
 subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-fsanitize=undefined','-I',str(ROOT),str(p/'test.c'),str(ROOT/'audio_nav_pacing.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
