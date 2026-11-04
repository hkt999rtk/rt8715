/* Generated from patch_carplay_nav_rx.py; r4 is the audited context.
 * Preserve native arguments/results and all callee-saved registers.
 * Forward all stack arguments; after callback receives saved input r1-r12 and return r0. */
.syntax unified
.thumb
.section .text.carbox_nav_app_hook_1,"ax",%progbits
.align 2
.global carbox_nav_app_hook_1
.type carbox_nav_app_hook_1,%function
.thumb_func
carbox_nav_app_hook_1:
 push {r0-r12,lr}
 mov r0,r4
 movs r1,#1
 mov r2,sp
 bl carbox_nav_app_before
 ldmia sp,{r0-r3}
 bl SetCurrentThreadPriority
 str r0,[sp]
 mov r0,r4
 movs r1,#1
 mov r2,sp
 bl carbox_nav_app_after
 pop {r0-r12,pc}
.size carbox_nav_app_hook_1,.-carbox_nav_app_hook_1
.section .text.carbox_nav_app_hook_2,"ax",%progbits
.align 2
.global carbox_nav_app_hook_2
.type carbox_nav_app_hook_2,%function
.thumb_func
carbox_nav_app_hook_2:
 push {r0-r12,lr}
 mov r0,r4
 movs r1,#2
 mov r2,sp
 bl carbox_nav_app_before
 ldmia sp,{r0-r3}
 sub sp,sp,#24
 ldr r12,[sp,#80]
 str r12,[sp,#0]
 ldr r12,[sp,#84]
 str r12,[sp,#4]
 ldr r12,[sp,#88]
 str r12,[sp,#8]
 ldr r12,[sp,#92]
 str r12,[sp,#12]
 ldr r12,[sp,#96]
 str r12,[sp,#16]
 ldr r12,[sp,#100]
 str r12,[sp,#20]
 bl SocketRecvFrom
 add sp,sp,#24
 str r0,[sp]
 mov r0,r4
 movs r1,#2
 mov r2,sp
 bl carbox_nav_app_after
 pop {r0-r12,pc}
.size carbox_nav_app_hook_2,.-carbox_nav_app_hook_2
.section .text.carbox_nav_app_hook_3,"ax",%progbits
.align 2
.global carbox_nav_app_hook_3
.type carbox_nav_app_hook_3,%function
.thumb_func
carbox_nav_app_hook_3:
 push {r0-r12,lr}
 mov r0,r4
 movs r1,#3
 mov r2,sp
 bl carbox_nav_app_before
 ldmia sp,{r0-r3}
 bl chacha20_poly1305_verify
 str r0,[sp]
 mov r0,r4
 movs r1,#3
 mov r2,sp
 bl carbox_nav_app_after
 pop {r0-r12,pc}
.size carbox_nav_app_hook_3,.-carbox_nav_app_hook_3
.section .text.carbox_nav_app_hook_4,"ax",%progbits
.align 2
.global carbox_nav_app_hook_4
.type carbox_nav_app_hook_4,%function
.thumb_func
carbox_nav_app_hook_4:
 push {r0-r12,lr}
 mov r0,r4
 movs r1,#4
 mov r2,sp
 bl carbox_nav_app_before
 ldmia sp,{r0-r3}
 bl RTPJitterBufferPutFreeNode
 str r0,[sp]
 mov r0,r4
 movs r1,#4
 mov r2,sp
 bl carbox_nav_app_after
 pop {r0-r12,pc}
.size carbox_nav_app_hook_4,.-carbox_nav_app_hook_4
.section .text.carbox_nav_app_hook_5,"ax",%progbits
.align 2
.global carbox_nav_app_hook_5
.type carbox_nav_app_hook_5,%function
.thumb_func
carbox_nav_app_hook_5:
 push {r0-r12,lr}
 mov r0,r4
 movs r1,#5
 mov r2,sp
 bl carbox_nav_app_before
 ldmia sp,{r0-r3}
 bl RTPJitterBufferPutBusyNode
 str r0,[sp]
 mov r0,r4
 movs r1,#5
 mov r2,sp
 bl carbox_nav_app_after
 pop {r0-r12,pc}
.size carbox_nav_app_hook_5,.-carbox_nav_app_hook_5
.section .text.carbox_nav_app_hook_6,"ax",%progbits
.align 2
.global carbox_nav_app_hook_6
.type carbox_nav_app_hook_6,%function
.thumb_func
carbox_nav_app_hook_6:
 push {r0-r12,lr}
 mov r0,r4
 movs r1,#6
 mov r2,sp
 bl carbox_nav_app_before
 ldmia sp,{r0-r3}
 bl pthread_exit
 str r0,[sp]
 mov r0,r4
 movs r1,#6
 mov r2,sp
 bl carbox_nav_app_after
 pop {r0-r12,pc}
.size carbox_nav_app_hook_6,.-carbox_nav_app_hook_6
.section .text.carbox_nav_app_hook_7,"ax",%progbits
.align 2
.global carbox_nav_app_hook_7
.type carbox_nav_app_hook_7,%function
.thumb_func
carbox_nav_app_hook_7:
 push {r0-r12,lr}
 mov r0,r4
 movs r1,#7
 mov r2,sp
 bl carbox_nav_app_before
 ldmia sp,{r0-r3}
 sub sp,sp,#8
 ldr r12,[sp,#64]
 str r12,[sp,#0]
 bl lwip_select
 add sp,sp,#8
 str r0,[sp]
 mov r0,r4
 movs r1,#7
 mov r2,sp
 bl carbox_nav_app_after
 pop {r0-r12,pc}
.size carbox_nav_app_hook_7,.-carbox_nav_app_hook_7
.section .note.GNU-stack,"",%progbits
