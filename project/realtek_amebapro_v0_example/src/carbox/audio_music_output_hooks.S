/* Only five validated AirPlayAudio.o relocations call these adapters.
 * R4 is the live stream in each native caller. No stack walking is used. */
#if CONFIG_AUDIO_MUSIC_DIAG
    .syntax unified
    .thumb
    .macro MUSIC_TAIL name, helper, arg
    .section .text.\name,"ax",%progbits
    .align 2
    .global \name
    .type \name,%function
    .thumb_func
\name:
    mov \arg, r4
    b.w \helper
    .size \name, .-\name
    .endm

    MUSIC_TAIL carbox_music_output_init, carbox_music_output_init_c, r2
    MUSIC_TAIL carbox_music_output_push, carbox_music_output_push_c, r3
    MUSIC_TAIL carbox_music_output_pop, carbox_music_output_pop_c, r3
    MUSIC_TAIL carbox_music_output_exit, carbox_music_output_exit_c, r1

    .section .text.carbox_music_output_sendto,"ax",%progbits
    .align 2
    .global carbox_music_output_sendto
    .type carbox_music_output_sendto,%function
    .thumb_func
carbox_music_output_sendto:
    push {r4, lr}
    sub sp, sp, #16
    /* Preserve native arguments 5/6, add stream as argument 7. SP stays
     * eight-byte aligned at BL; native r0-r3 and caller stack are untouched. */
    ldr ip, [sp, #24]
    str ip, [sp, #0]
    ldr ip, [sp, #28]
    str ip, [sp, #4]
    str r4, [sp, #8]
    bl carbox_music_output_sendto_c
    add sp, sp, #16
    pop {r4, pc}
    .size carbox_music_output_sendto, .-carbox_music_output_sendto
#if CONFIG_AUDIO_NAV_PACING
    /* Native post-send BL at +0x118 returns to +0x11c. On handled navigation
     * resume at loop/stop check +0x90 (LR - 0x8c); otherwise tail-call exactly
     * the replaced RingBuffer_size and continue the original scheduling block.
     * Original object hash and function size are pinned by the archive patch. */
    .section .text.carbox_nav_output_pace,"ax",%progbits
    .align 2
    .global carbox_nav_output_pace
    .type carbox_nav_output_pace,%function
    .thumb_func
carbox_nav_output_pace:
    push {r0, r4, r5, lr}
    mov r1, r4
    mov r2, r5
    bl carbox_nav_output_pace_c
    cmp r0, #0
    beq 1f
    pop {r0, r4, r5, lr}
    sub.w lr, lr, #0x8c
    bx lr
1:
    pop {r0, r4, r5, lr}
    b.w RingBuffer_size
    .size carbox_nav_output_pace, .-carbox_nav_output_pace
#endif
#endif
    .section .note.GNU-stack,"",%progbits
