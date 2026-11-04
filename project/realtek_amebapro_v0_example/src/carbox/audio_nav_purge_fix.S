/* Audited AirPlayUtils.o RTPJitterBufferRead+0x6e call-site replacement.
 * The context semaphore is held by Read. DiscardExcess may recycle its saved
 * r5 head; reacquire the live head before the native continuation reads its TS.
 * This is a private call-site ABI: r5 is intentionally changed.
 */
.syntax unified
.thumb
.section .text.carbox_nav_purge_read,"ax",%progbits
.align 2
.global carbox_nav_purge_read
.type carbox_nav_purge_read,%function
.thumb_func
carbox_nav_purge_read:
 push {r0-r12,lr}
#if CONFIG_AUDIO_NAV_DIAG
 /* The diagnostic wrapper calls native purge, then returns normally. Keeping
  * it inside this trampoline retains the Read return PC for the empty case. */
 bl carbox_nav_hook_21
#else
 bl _RTPJitterBufferDiscardExcess
#endif
 stmia sp,{r0-r3}
 ldr r5,[r4,#0x138]
 cbnz r5,1f
 ldr r5,[r4,#0x140]
 cbnz r5,1f
 /* No node remains: retain rebuffer state and take Read's existing whole-output
  * zero/wait path at +0x48. r8 is still requested sample count here. */
 ldr r5,[r4,#0x188]
 mul r5,r8,r5
 ldr r0,[sp,#52]
 sub r0,r0,#0x2a /* caller LR = Read+0x72|1, wait path = Read+0x48|1 */
 str r0,[sp,#52]
1:
 str r5,[sp,#20]
 pop {r0-r12,pc}
.size carbox_nav_purge_read,.-carbox_nav_purge_read
.section .note.GNU-stack,"",%progbits
