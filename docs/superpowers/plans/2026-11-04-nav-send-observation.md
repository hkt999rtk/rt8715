# Navigation send and onset observation

Approved: add evidence without changing pacing, PCM, codec concealment or timeout behavior.

- Trace navigation output epoch/RTP sequence/timestamp through socket allocation/copy, core-lock wait/hold, NCM enqueue/dequeue/build, USB HAL entry/return.
- Preserve all frame identities across NTB batches, including partial prefixes. Use generation checked handles, fixed LPDDR storage, no hot-path allocation/printing. Report lost/expired/unmatched observations.
- Core-lock owner is the owner observed when waiting begins, not proof it held the lock for the entire wait. USB HAL return is not peer playback or an on-wire timestamp.
- Deferred-neighbor/fragmented packets not attached synchronously to NCM are explicitly unobserved, never reported as USB successes.
- Capture first-second pacing/queue statistics and actual FDK concealment state transitions using the existing library function ABI; do not alter fade parameters.
- Host-test record ordering, stale handles, wrap, overload and onset boundaries; review hooks; build and verify final ELF/image/hash.
