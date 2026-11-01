/* ABI-bound observation only. See verify_recvframe_trace.py before changing.
 * The native free function saves r0,r1,r4-r8,lr (32 bytes). At its list-insert
 * call the saved caller LR is SP+28. Other callers MUST NOT read that slot.
 * No stack adjustment, callee-saved register change, or queue policy change. */
#if CONFIG_RX_HANDOVER_DIAG
    .syntax unified
    .thumb
    .section .text.__wrap_rtw_list_insert_tail,"ax",%progbits
    .align 2
    .global __wrap_rtw_list_insert_tail
    .type __wrap_rtw_list_insert_tail,%function
    .thumb_func
__wrap_rtw_list_insert_tail:
    mov r2, lr
    ldr r3, .Lfree_return
    cmp r2, r3
    ite eq
    ldreq r3, [sp, #28]
    movne r3, #0
    b.w carbox_recvframe_insert
    .align 2
.Lfree_return:
    .word rtw_free_recvframe + 0x46
    .size __wrap_rtw_list_insert_tail, .-__wrap_rtw_list_insert_tail
#endif
    .section .note.GNU-stack,"",%progbits
