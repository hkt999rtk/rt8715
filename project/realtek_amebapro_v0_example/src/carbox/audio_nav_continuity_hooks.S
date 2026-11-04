/* Native register ABI is supplied by the hash-pinned NAV hooks, which preserve
 * r4/r5/r8. Converter stack arguments follow the 56-byte register save area. */
.syntax unified
.thumb
.section .text.carbox_nav_convert_hook,"ax",%progbits
.align 2
.global carbox_nav_convert_hook
.type carbox_nav_convert_hook,%function
.thumb_func
carbox_nav_convert_hook:
 push {r0-r12,lr}
 mov r0,r4
 mov r1,sp
 bl carbox_nav_convert_saved
 str r0,[sp]
 pop {r0-r12,pc}
.size carbox_nav_convert_hook,.-carbox_nav_convert_hook
.macro gap_hook name, reason
.section .text.\name,"ax",%progbits
.align 2
.global \name
.type \name,%function
.thumb_func
\name:
 push {r0-r12,lr}
 mov r0,r4
 mov r1,sp
 movs r2,#\reason
 bl carbox_nav_gap_saved
 str r0,[sp]
 pop {r0-r12,pc}
.size \name,.-\name
.endm
gap_hook carbox_nav_future_hook,11
gap_hook carbox_nav_gap_hook,13
.section .note.GNU-stack,"",%progbits
