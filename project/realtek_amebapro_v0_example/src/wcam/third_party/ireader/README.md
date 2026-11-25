# ireader H.264 RTP subset

Source: https://github.com/ireader/media-server
Commit: c0463893bc509e65bea9f4f76be68c1f89ccdfcf (2026-10-10)
License: MIT, retained in LICENSE.

Only H.264 packetization/depacketization and RTP header code are compiled.
Local changes: TX marker also covers a trailing non-VCL NAL; unpacker uses caller-owned bounded AU storage instead of realloc;
only mode 1 (Single NAL/STAP-A/FU-A) accepted; validation rejects malformed
fragments and aggregates. Exported symbols prefixed wcam_ir_ at compile time to
avoid collisions with the Realtek lib_rtsp.a. Socket/thread code is not imported.
