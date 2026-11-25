# Wireless wcam Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build separate wireless TX and USB UVC RX firmware images.
**Architecture:** Portable bounded RTP/RTCP core with ireader H.264 pack/unpack;
FreeRTOS/lwIP adapter, RX AP/TX STA; compile-time role selection with clean builds.
**Tech Stack:** C99, FreeRTOS, lwIP, Realtek GCC 6.4.1, MIT ireader media-server.
**Spec:** docs/superpowers/specs/2026-11-04-wcam-wireless.md

## Global Constraints
- Preserve SDK USB ABI fixes and 720p30 profile; no customer archives.
- RX AP, TX automatic retry; deterministic memory ownership; bounded queues.
- Build sequential LP then IS using repository AGENTS.md. Publish only successful outputs.

## Review Focus
- Missing middle/end packets and reordered sequence wrap: never output corrupt AU.
- TX reboot/SSRC change and UVC stop/start: recover with a fresh parameterized IDR.
- Malformed/truncated RTP/RTCP and capacity overflow: reject without memory corruption.
- USB timeout: retain DMA memory; network task must never overwrite it.
- Role switching: stale object files must not produce mislabeled images.

### Task 1: Portable transport
Files: src/wcam/third_party/ireader/*, src/wcam/wcam_rtp.[ch], wcam/tests/test_rtp.c.
- [x] Add tests for real Annex B round trip, fragmentation, reordering/loss, wrap,
  malformed inputs, bounded capacity and RTCP PLI addressing.
- [x] Observe RED, port core with provenance and fixed buffers, reach GREEN with sanitizers.
- [x] Run wcam/tests/run.sh.

### Task 2: Firmware adapters
Files: src/wcam/wcam_net.[ch], wcam_tx.c, wcam_uvc.c,
wcam_config.h, src/main.c, application.is.mk, platform_opts.h.
- [x] Add Wi-Fi AP/STA startup and infinite reconnect; fixed UDP ports/IPs.
- [x] TX packetization + RTCP feedback; RX dedicated receive task and owned AU buffers.
- [x] RX UVC consumes complete frames and requests fresh IDR after start/loss.
- [x] Logs cover role/link/packets/loss/PLI/frame/drop and USB/encoder timing.

### Task 3: Build and delivery
Files: wcam/build-pair.sh, wcam/README.md, wcam/flash_wcam_{tx,rx}.bin + checksums.
- [x] Full host regression, independent final review, sequential clean builds per role.
- [x] Verify ELF roles/provenance, successful packaging, timestamps and SHA256.
- [x] Document pairing, boot expectations, exact paths and hardware validation limit.

## Execution notes
User authorized implementation and selected RX AP / TX retry. Execute inline in
this existing wcam checkout; no additional design approval needed.

### Review and evidence
- Independent review: review_wireless identified trailing non-VCL marker,
  restart generation validation, signed jitter overflow, idle sequence recovery,
  and destination-role clean requirements. Addressed in this port.
- Missing/partial FU with marker regression reproduced before fix; fixed.
- Marker, idle recovery, RR loss and UBSan jitter regressions reproduced before fixes.
- Host transport and existing pattern/control/stats/pacer suites pass.
- Real board capture: 270 complete AUs round-trip byte-for-byte and decode using FFmpeg.
- SDK LwIP_Init referenced a newer netif_get_index absent from v2.0.2; log now
  reads the existing num field and explicitly includes netifapi.h.
- Hardware validation is pending; no target boards are attached to this session.

- Final malformed-FU-start regression reproduced an incomplete multi-slice AU
  escaping as a prefix; rejecting discarded depacketizer inputs fixed it.
- Both roles passed clean build + ELF checks. RX rebuilt incrementally after
  this final receive-only fix, preserving its matching LP image.
