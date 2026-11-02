A-MSDU child receive-frame return repair
======================================

amsdu_to_msdu() allocates a child from the primary free pool while processing
on a buddy adapter. The child retained its old adapter, so the native free
routine could put it on the buddy queue without restoring the primary count.
The available primary pool gradually drained. audio-lag5.log identified 31
different children returned this way, reducing the home pool from 32 to 1.

The derived WLAN archive now redirects the child's list-initialization call
to wlan_amsdu_child_init. This private 8-byte Thumb bridge initializes
child->adapter from the processing adapter before calling the native list
initializer. The native allocation and free rules remain in use, including
failure cleanup. This repair does not depend on diagnostics being enabled.

Version constraints:
  rtw_recv.o after the existing NULL-allocation and 32-frame patches:
  a6dc25893e3fb17e8e0a8a4dd0fc1fe08e1a258c267cf73374c294689e8f6e4a
  rtw_recv.o after this repair:
  452b1a0782e9ac7f0bb6cb7d57db72f375e2af15f711b640174e1e269cea0f8d
  amsdu_to_msdu+0x76: r0=child, r8=processing adapter, adapter field=child+0x10.
Unknown input versions are rejected. The original vendor archive is preserved.
The build verifies the archive and the final linked helper call/tail-call.
WLAN_RX_DMA_SKB=0 selects the original archive and does not include this fix.

Verification:
  test_amsdu_child_adapter.py executes the actual vendor Thumb routines with
  controlled external dependencies (host Python unicorn and pyelftools).
  Covers both adapter roles, allocation/indication failures, actual pool
  exhaustion and recovery, ordinary traffic, ABI and patch corruption checks.
  10,000 A-MSDUs / 20,000 children restored both native queues and counts.
  User reported the diagnostic firmware test passed on 2026-10-30.

Release defaults in application.is.mk:
  RX_HANDOVER_DIAG=0, AUDIO_STREAM_DIAG=0, AAC_ELD_DIAG=0.
  CPU clock, zero-copy, pool size and audio routing retain their prior settings.
  RX observer code/state is omitted when diagnostics are disabled. The startup
  task still initializes the shared printf mutex, then deletes itself.
  Opt-in with make variables when diagnosis is required; changing these flags
  rebuilds the affected objects. The repair itself allocates/copies/logs nothing.

When a new vendor library incorporates the source fix, review its ABI and
lifetime handling, retire this bridge/relocation patch, and update the guards.

GCC lib_wlan_mp.a (2026-11-02)
-----------------------------
The MP archive has the same missing child->adapter initialization, but a
different adapter layout: buddy/type at 0x3a20/0x3a32 (normal: 0x3378/0x338a).
The child adapter field remains +0x10, processing adapter remains r8, and
the list-init call remains amsdu_to_msdu+0x76. The NULL-allocation failure
also branches to the same trap at +0x260. These were checked against MP
Thumb disassembly, not inferred from the normal archive's offsets.

Build from component/common/drivers/wlan/realtek/wlan_rx_gdma:
  make mp-fix CROSS_COMPILE=/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-
Output: build/lib_wlan_mp.a
The all target also builds it. The normal firmware's zero-copy target still
links build/lib_wlan_zero_copy.a; building the MP repair does not switch it.

Original MP archive is preserved at:
  component/soc/realtek/8195b/misc/bsp/lib/common/GCC/lib_wlan_mp.a
The derived MP archive changes only rtw_recv.o and adds the 8-byte bridge.
It retains MP's 16-frame pool, native RX/skbuff objects and MP functionality.
It includes both the NULL-allocation return fix and child adapter repair.
Do not use the normal driver's recovered RX/skbuff objects with the MP ABI.
This patch does not cover the separately supplied IAR archive.

Pinned SHA-256 identities:
  original GCC MP archive:
    cab792c356dfece332ea395784163a369144198917e95597564e00c0630c26dd
  original MP rtw_recv.o:
    be051194bc70209f54c77b05a3b6898786fa445942e977cf6d55901845950525
  MP rtw_recv.o after NULL-allocation repair (input to child repair):
    688b48387f11c185815c0134c4b60a1dc3f91cb73c4ec537178d7499b5fe4a4b
  MP rtw_recv.o after child repair:
    9742e0351020e58c67e116fa0650eefcb2a96314cf84dbce8be9fd24614d26a2
  native MP freertos_recv.o:
    600fb25fa9ec698342ffd8db6845b3ac727cd425e7d24f9eaa0e2ea4ff05aa33
Verifier pairs rtw_recv.o and freertos_recv.o identities to reject mixed ABIs.
Builder rejects unknown original archives and verifies every untouched member.

MP native-code regression:
  test_amsdu_child_adapter.py --mp --vendor-archive <original GCC MP archive> --prefix <toolchain prefix>
  With --unfixed, one A-MSDU reproduces A=14, B=18, A_count=14, B_count=16.
  Fixed code passes 40 role/failure cases, actual pool exhaustion/recovery,
  10,000 A-MSDUs / 20,000 children, ABI/lifetime and corruption checks.
  These are host Thumb emulation tests with stubbed hardware dependencies;
  on-board MP/AP+STA long-duration testing remains required.
