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
