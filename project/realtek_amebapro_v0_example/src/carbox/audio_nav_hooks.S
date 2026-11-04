/* Generated from patch_carplay_nav_diag.py; r4 is the audited context.
 * Preserve native arguments/results and all callee-saved registers.
 * Converter arguments 5/6 remain at the callee stack top. */
.syntax unified
.thumb
.section .text.carbox_nav_hook_1,"ax",%progbits
.align 2
.global carbox_nav_hook_1
.type carbox_nav_hook_1,%function
.thumb_func
carbox_nav_hook_1:
 push {r0-r12,lr}
 mov r0,r4
 movs r1,#1
 mov r2,sp
 bl carbox_nav_before
 ldmia sp,{r0-r3}
#if CONFIG_AUDIO_NAV_PLAYOUT
 bl carbox_nav_condition_wait
#else
 bl pthread_cond_wait
#endif
 stmia sp,{r0-r3}
 mov r0,r4
 movs r1,#1
 mov r2,sp
 bl carbox_nav_after
 pop {r0-r12,pc}
.size carbox_nav_hook_1,.-carbox_nav_hook_1
.section .text.carbox_nav_hook_2,"ax",%progbits
.align 2
.global carbox_nav_hook_2
.type carbox_nav_hook_2,%function
.thumb_func
carbox_nav_hook_2:
 push {r0-r12,lr}
 mov r0,r4
 movs r1,#2
 mov r2,sp
 bl carbox_nav_before
 ldmia sp,{r0-r3}
 bl dispatch_semaphore_wait
 stmia sp,{r0-r3}
 mov r0,r4
 movs r1,#2
 mov r2,sp
 bl carbox_nav_after
 pop {r0-r12,pc}
.size carbox_nav_hook_2,.-carbox_nav_hook_2
.section .text.carbox_nav_hook_3,"ax",%progbits
.align 2
.global carbox_nav_hook_3
.type carbox_nav_hook_3,%function
.thumb_func
carbox_nav_hook_3:
 push {r0-r12,lr}
 mov r0,r4
 movs r1,#3
 mov r2,sp
 bl carbox_nav_before
 ldmia sp,{r0-r3}
 bl dispatch_semaphore_wait
 stmia sp,{r0-r3}
 mov r0,r4
 movs r1,#3
 mov r2,sp
 bl carbox_nav_after
 pop {r0-r12,pc}
.size carbox_nav_hook_3,.-carbox_nav_hook_3
.section .text.carbox_nav_hook_4,"ax",%progbits
.align 2
.global carbox_nav_hook_4
.type carbox_nav_hook_4,%function
.thumb_func
carbox_nav_hook_4:
 push {r0-r12,lr}
 mov r0,r4
 movs r1,#4
 mov r2,sp
 bl carbox_nav_before
 ldmia sp,{r0-r3}
 ldr r12,[sp,#56]
 sub sp,sp,#8
 str r12,[sp]
 ldr r12,[sp,#68]
 str r12,[sp,#4]
#if CONFIG_AUDIO_NAV_CONTINUITY
 bl carbox_nav_convert_hook
#else
 bl AudioConverterFillComplexBuffer
#endif
 add sp,sp,#8
 stmia sp,{r0-r3}
 mov r0,r4
 movs r1,#4
 mov r2,sp
 bl carbox_nav_after
 pop {r0-r12,pc}
.size carbox_nav_hook_4,.-carbox_nav_hook_4
.section .text.carbox_nav_hook_5,"ax",%progbits
.align 2
.global carbox_nav_hook_5
.type carbox_nav_hook_5,%function
.thumb_func
carbox_nav_hook_5:
 push {r0-r12,lr}
 mov r0,r4
 movs r1,#5
 mov r2,sp
 bl carbox_nav_before
 ldmia sp,{r0-r3}
 bl dispatch_semaphore_wait
 stmia sp,{r0-r3}
 mov r0,r4
 movs r1,#5
 mov r2,sp
 bl carbox_nav_after
 pop {r0-r12,pc}
.size carbox_nav_hook_5,.-carbox_nav_hook_5
.section .text.carbox_nav_hook_6,"ax",%progbits
.align 2
.global carbox_nav_hook_6
.type carbox_nav_hook_6,%function
.thumb_func
carbox_nav_hook_6:
 push {r0-r12,lr}
 mov r0,r4
 movs r1,#6
 mov r2,sp
 bl carbox_nav_before
 ldmia sp,{r0-r3}
 bl dispatch_semaphore_signal
 stmia sp,{r0-r3}
 mov r0,r4
 movs r1,#6
 mov r2,sp
 bl carbox_nav_after
 pop {r0-r12,pc}
.size carbox_nav_hook_6,.-carbox_nav_hook_6
.section .text.carbox_nav_hook_7,"ax",%progbits
.align 2
.global carbox_nav_hook_7
.type carbox_nav_hook_7,%function
.thumb_func
carbox_nav_hook_7:
 push {r0-r12,lr}
 mov r0,r4
 movs r1,#7
 mov r2,sp
 bl carbox_nav_before
 ldmia sp,{r0-r3}
 bl dispatch_semaphore_signal
 stmia sp,{r0-r3}
 mov r0,r4
 movs r1,#7
 mov r2,sp
 bl carbox_nav_after
 pop {r0-r12,pc}
.size carbox_nav_hook_7,.-carbox_nav_hook_7
.section .text.carbox_nav_hook_8,"ax",%progbits
.align 2
.global carbox_nav_hook_8
.type carbox_nav_hook_8,%function
.thumb_func
carbox_nav_hook_8:
 push {r0-r12,lr}
 mov r0,r4
 movs r1,#8
 mov r2,sp
 bl carbox_nav_before
 ldmia sp,{r0-r3}
 bl dispatch_semaphore_wait
 stmia sp,{r0-r3}
 mov r0,r4
 movs r1,#8
 mov r2,sp
 bl carbox_nav_after
 pop {r0-r12,pc}
.size carbox_nav_hook_8,.-carbox_nav_hook_8
.section .text.carbox_nav_hook_9,"ax",%progbits
.align 2
.global carbox_nav_hook_9
.type carbox_nav_hook_9,%function
.thumb_func
carbox_nav_hook_9:
 push {r0-r12,lr}
 mov r0,r4
 movs r1,#9
 mov r2,sp
 bl carbox_nav_before
 ldmia sp,{r0-r3}
 bl dispatch_semaphore_signal
 stmia sp,{r0-r3}
 mov r0,r4
 movs r1,#9
 mov r2,sp
 bl carbox_nav_after
 pop {r0-r12,pc}
.size carbox_nav_hook_9,.-carbox_nav_hook_9
.section .text.carbox_nav_hook_10,"ax",%progbits
.align 2
.global carbox_nav_hook_10
.type carbox_nav_hook_10,%function
.thumb_func
carbox_nav_hook_10:
 push {r0-r12,lr}
 mov r0,r4
 movs r1,#10
 mov r2,sp
 bl carbox_nav_before
 ldmia sp,{r0-r3}
 bl memset
 stmia sp,{r0-r3}
 mov r0,r4
 movs r1,#10
 mov r2,sp
 bl carbox_nav_after
 pop {r0-r12,pc}
.size carbox_nav_hook_10,.-carbox_nav_hook_10
.section .text.carbox_nav_hook_11,"ax",%progbits
.align 2
.global carbox_nav_hook_11
.type carbox_nav_hook_11,%function
.thumb_func
carbox_nav_hook_11:
 push {r0-r12,lr}
 mov r0,r4
 movs r1,#11
 mov r2,sp
 bl carbox_nav_before
 ldmia sp,{r0-r3}
#if CONFIG_AUDIO_NAV_CONTINUITY
 bl carbox_nav_future_hook
#else
 bl memset
#endif
 stmia sp,{r0-r3}
 mov r0,r4
 movs r1,#11
 mov r2,sp
 bl carbox_nav_after
 pop {r0-r12,pc}
.size carbox_nav_hook_11,.-carbox_nav_hook_11
.section .text.carbox_nav_hook_12,"ax",%progbits
.align 2
.global carbox_nav_hook_12
.type carbox_nav_hook_12,%function
.thumb_func
carbox_nav_hook_12:
 push {r0-r12,lr}
 mov r0,r4
 movs r1,#12
 mov r2,sp
 bl carbox_nav_before
 ldmia sp,{r0-r3}
 bl memset
 stmia sp,{r0-r3}
 mov r0,r4
 movs r1,#12
 mov r2,sp
 bl carbox_nav_after
 pop {r0-r12,pc}
.size carbox_nav_hook_12,.-carbox_nav_hook_12
.section .text.carbox_nav_hook_13,"ax",%progbits
.align 2
.global carbox_nav_hook_13
.type carbox_nav_hook_13,%function
.thumb_func
carbox_nav_hook_13:
 push {r0-r12,lr}
 mov r0,r4
 movs r1,#13
 mov r2,sp
 bl carbox_nav_before
 ldmia sp,{r0-r3}
#if CONFIG_AUDIO_NAV_CONTINUITY
 bl carbox_nav_gap_hook
#else
 bl memset
#endif
 stmia sp,{r0-r3}
 mov r0,r4
 movs r1,#13
 mov r2,sp
 bl carbox_nav_after
 pop {r0-r12,pc}
.size carbox_nav_hook_13,.-carbox_nav_hook_13
.section .text.carbox_nav_hook_14,"ax",%progbits
.align 2
.global carbox_nav_hook_14
.type carbox_nav_hook_14,%function
.thumb_func
carbox_nav_hook_14:
 push {r0-r12,lr}
 mov r0,r4
 movs r1,#14
 mov r2,sp
 bl carbox_nav_before
 ldmia sp,{r0-r3}
 bl _RTPJitterBufferBufferedSamples
 stmia sp,{r0-r3}
 mov r0,r4
 movs r1,#14
 mov r2,sp
 bl carbox_nav_after
 pop {r0-r12,pc}
.size carbox_nav_hook_14,.-carbox_nav_hook_14
.section .text.carbox_nav_hook_15,"ax",%progbits
.align 2
.global carbox_nav_hook_15
.type carbox_nav_hook_15,%function
.thumb_func
carbox_nav_hook_15:
 push {r0-r12,lr}
 mov r0,r4
 movs r1,#15
 mov r2,sp
 bl carbox_nav_before
 ldmia sp,{r0-r3}
 bl dispatch_semaphore_wait
 stmia sp,{r0-r3}
 mov r0,r4
 movs r1,#15
 mov r2,sp
 bl carbox_nav_after
 pop {r0-r12,pc}
.size carbox_nav_hook_15,.-carbox_nav_hook_15
.section .text.carbox_nav_hook_16,"ax",%progbits
.align 2
.global carbox_nav_hook_16
.type carbox_nav_hook_16,%function
.thumb_func
carbox_nav_hook_16:
 push {r0-r12,lr}
 mov r0,r4
 movs r1,#16
 mov r2,sp
 bl carbox_nav_before
 ldmia sp,{r0-r3}
 bl dispatch_semaphore_signal
 stmia sp,{r0-r3}
 mov r0,r4
 movs r1,#16
 mov r2,sp
 bl carbox_nav_after
 pop {r0-r12,pc}
.size carbox_nav_hook_16,.-carbox_nav_hook_16
.section .text.carbox_nav_hook_17,"ax",%progbits
.align 2
.global carbox_nav_hook_17
.type carbox_nav_hook_17,%function
.thumb_func
carbox_nav_hook_17:
 push {r0-r12,lr}
 mov r0,r4
 movs r1,#17
 mov r2,sp
 bl carbox_nav_before
 ldmia sp,{r0-r3}
#if CONFIG_AUDIO_NAV_PLAYOUT
 bl carbox_nav_condition_signal
#else
 bl pthread_cond_signal
#endif
 stmia sp,{r0-r3}
 mov r0,r4
 movs r1,#17
 mov r2,sp
 bl carbox_nav_after
 pop {r0-r12,pc}
.size carbox_nav_hook_17,.-carbox_nav_hook_17
.section .text.carbox_nav_hook_18,"ax",%progbits
.align 2
.global carbox_nav_hook_18
.type carbox_nav_hook_18,%function
.thumb_func
carbox_nav_hook_18:
 push {r0-r12,lr}
 mov r0,r4
 movs r1,#18
 mov r2,sp
 bl carbox_nav_before
 ldmia sp,{r0-r3}
#if CONFIG_AUDIO_NAV_PLAYOUT
 bl carbox_nav_condition_signal
#else
 bl pthread_cond_signal
#endif
 stmia sp,{r0-r3}
 mov r0,r4
 movs r1,#18
 mov r2,sp
 bl carbox_nav_after
 pop {r0-r12,pc}
.size carbox_nav_hook_18,.-carbox_nav_hook_18
.section .text.carbox_nav_hook_19,"ax",%progbits
.align 2
.global carbox_nav_hook_19
.type carbox_nav_hook_19,%function
.thumb_func
carbox_nav_hook_19:
 push {r0-r12,lr}
 mov r0,r4
 movs r1,#19
 mov r2,sp
 bl carbox_nav_before
 ldmia sp,{r0-r3}
 bl _RTPJitterBufferBufferedSamples
 stmia sp,{r0-r3}
 mov r0,r4
 movs r1,#19
 mov r2,sp
 bl carbox_nav_after
 pop {r0-r12,pc}
.size carbox_nav_hook_19,.-carbox_nav_hook_19
.section .text.carbox_nav_hook_20,"ax",%progbits
.align 2
.global carbox_nav_hook_20
.type carbox_nav_hook_20,%function
.thumb_func
carbox_nav_hook_20:
 push {r0-r12,lr}
 mov r0,r4
 movs r1,#20
 mov r2,sp
 bl carbox_nav_before
 ldmia sp,{r0-r3}
 bl _RTPJitterBufferDiscardExcess
 stmia sp,{r0-r3}
 mov r0,r4
 movs r1,#20
 mov r2,sp
 bl carbox_nav_after
 pop {r0-r12,pc}
.size carbox_nav_hook_20,.-carbox_nav_hook_20
.section .text.carbox_nav_hook_21,"ax",%progbits
.align 2
.global carbox_nav_hook_21
.type carbox_nav_hook_21,%function
.thumb_func
carbox_nav_hook_21:
 push {r0-r12,lr}
 mov r0,r4
 movs r1,#21
 mov r2,sp
 bl carbox_nav_before
 ldmia sp,{r0-r3}
#if CONFIG_AUDIO_NAV_CONTINUITY
 bl carbox_nav_startup_purge
#else
 bl _RTPJitterBufferDiscardExcess
#endif
 stmia sp,{r0-r3}
 mov r0,r4
 movs r1,#21
 mov r2,sp
 bl carbox_nav_after
 pop {r0-r12,pc}
.size carbox_nav_hook_21,.-carbox_nav_hook_21
.section .text.carbox_nav_hook_22,"ax",%progbits
.align 2
.global carbox_nav_hook_22
.type carbox_nav_hook_22,%function
.thumb_func
carbox_nav_hook_22:
 push {r0-r12,lr}
 mov r0,r4
 movs r1,#22
 mov r2,sp
 bl carbox_nav_before
 ldmia sp,{r0-r3}
 bl pthread_mutex_lock
 stmia sp,{r0-r3}
 mov r0,r4
 movs r1,#22
 mov r2,sp
 bl carbox_nav_after
 pop {r0-r12,pc}
.size carbox_nav_hook_22,.-carbox_nav_hook_22
.section .text.carbox_nav_hook_23,"ax",%progbits
.align 2
.global carbox_nav_hook_23
.type carbox_nav_hook_23,%function
.thumb_func
carbox_nav_hook_23:
 push {r0-r12,lr}
 mov r0,r4
 movs r1,#23
 mov r2,sp
 bl carbox_nav_before
 ldmia sp,{r0-r3}
 bl pthread_mutex_lock
 stmia sp,{r0-r3}
 mov r0,r4
 movs r1,#23
 mov r2,sp
 bl carbox_nav_after
 pop {r0-r12,pc}
.size carbox_nav_hook_23,.-carbox_nav_hook_23
.section .text.carbox_nav_hook_24,"ax",%progbits
.align 2
.global carbox_nav_hook_24
.type carbox_nav_hook_24,%function
.thumb_func
carbox_nav_hook_24:
 push {r0-r12,lr}
 mov r0,r4
 movs r1,#24
 mov r2,sp
 bl carbox_nav_before
 ldmia sp,{r0-r3}
 bl pthread_mutex_lock
 stmia sp,{r0-r3}
 mov r0,r4
 movs r1,#24
 mov r2,sp
 bl carbox_nav_after
 pop {r0-r12,pc}
.size carbox_nav_hook_24,.-carbox_nav_hook_24
.section .text.carbox_nav_hook_25,"ax",%progbits
.align 2
.global carbox_nav_hook_25
.type carbox_nav_hook_25,%function
.thumb_func
carbox_nav_hook_25:
 push {r0-r12,lr}
 mov r0,r4
 movs r1,#25
 mov r2,sp
 bl carbox_nav_before
 ldmia sp,{r0-r3}
 bl _RTPJitterBufferBufferedSamples
 stmia sp,{r0-r3}
 mov r0,r4
 movs r1,#25
 mov r2,sp
 bl carbox_nav_after
 pop {r0-r12,pc}
.size carbox_nav_hook_25,.-carbox_nav_hook_25
.section .note.GNU-stack,"",%progbits
