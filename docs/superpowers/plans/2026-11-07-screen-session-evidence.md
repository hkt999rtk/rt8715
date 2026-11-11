# Screen session cleanup and initialization evidence

> Execute inline using executing-plans; user authorized implementation and test firmware.

**Goal:** Stop and release an old screen sender safely; distinguish missing HID setup from video backpressure.
**Architecture:** Validated v17 archive symbol/relocation hooks, original archive unchanged. Keep 4KB writes, 250ms poll, encryption and frame representation. Reuse the existing reporting thread.
**Spec:** Approved conversation plan, with subsequent clarification: NAL packaging remains unchanged until runtime composition is observed.
**Constraints:** No automatic HID retry/reconnect, no zero-window disconnect deadline, no hot-path UART dumps, simplified Chinese comments; no commit/push requested.

## Review focus
- Stop while a partial encrypted frame is pending: cancel the session, never append a new frame to its suffix.
- Old sender and queued producer must not access a replacement socket/session.
- Socket cleanup must still run after send flag cleared or control channel disconnected.
- Missing HID key, wrong type, empty array and malformed plist must be distinguishable.
- NAL diagnostics must not modify input or count emulation-prevention sequences as boundaries.

## Tasks
- [x] Tests: host C lifecycle/write/observer tests and vendor relocation/ABI checks; record failures before implementation.
- [x] Implementation: screen-specific cancellable write/poll, joinable sender, serialized start/stop, cleanup after sender exits. No unlimited write(0) loop.
- [x] Evidence: /info HTTP metadata and parser outcome; HID typed lookup; NAL counts at send boundary; session events via existing report thread.
- [x] Verify host tests, archive changes and final linked calls; build with MOVE_LAG_PROFILE=1 and SCREEN_SESSION_FIX=1; verify flash image and SHA256.

## Decisions / evidence
- NAL rewrite deferred after user questioned assumed pass-through: observe actual encrypt input first. H264 data remains untouched.
- Fix hooks are opt-in SCREEN_SESSION_FIX=1 for this test image. Reference source changes will be documented against the C implementation, not silently assumed present in vendor library.

- Independent review found NULL sender/NULL pthread_self false self-join; host test reproduced failure, non-NULL guard now passes. Archive binding test also caught local-to-global rename ordering before final build.

## Verification result
- Host lifecycle/NAL/info tests and archive ABI/relocation tests passed.
- Existing MOVE, CAR_RX, HTTP ownership and screen poll tests passed.
- Final ELF confirms normal-frame write/poll/observer hooks, StartScreen create/joinable hooks, thread-exit hook, /info parser/HID lookup hooks, and existing lwip observer chain. Original EncryptData still called.
- Incremental ram_is build succeeded with external Realtek toolchain. git diff --check passed.
- flash_is.bin: 8298496 bytes; SHA256 1f3c8b105851a4790a7ccf8c73a16aa67b0693ba56065ca59778466ebe6faff3.
- Hardware/vehicle verification remains pending; NAL mismatch is a hypothesis, not a demonstrated root cause.
