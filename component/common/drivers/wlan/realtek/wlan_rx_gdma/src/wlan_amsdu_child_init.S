/* Private bridge for the hash-pinned vendor amsdu_to_msdu+0x76 call.
 * r0 = non-NULL child, r8 = processing adapter (callee-saved in vendor code).
 * This is NOT a C-callable API. Keep the original list-init calling contract.
 * Native rtw_free_recvframe uses this adapter's buddy policy to return a
 * secondary-interface child to the primary pool from which it was allocated.
 */
.syntax unified
.thumb
.section .wlan.text.wlan_amsdu_child_init,"ax",%progbits
.align 2
.global wlan_amsdu_child_init
.type wlan_amsdu_child_init,%function
.thumb_func
wlan_amsdu_child_init:
    str.w r8, [r0, #16]
    b.w rtw_init_listhead
.size wlan_amsdu_child_init, .-wlan_amsdu_child_init
.section .note.GNU-stack,"",%progbits
