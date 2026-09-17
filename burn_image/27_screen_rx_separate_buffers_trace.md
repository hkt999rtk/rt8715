# Screen RX separate-buffer feasibility trace

Read-only code/archive review, 2026-09-16. No firmware behavior changed.

## Finding

The screen consumer accepts a pointer and length; it does not require that
the pointer be the original ciphertext allocation. However, the existing
closed receiver currently passes identical source and destination pointers
to ChaCha and continues using that pointer after verify. Changing only the
hardware destination cannot redirect this caller's later use or cleanup.

## Evidence from the linked receiver

`AirPlayReceiverSessionScreen_ProcessFrames` in
`lib_CarPlay.a(AirPlayReceiverSessionScreen.o)`:

- `+0x16c`: allocate wire payload; result kept in R5.
- `+0x20e`: call `chacha20_poly1305_decrypt` with R1=R5 (source),
  R3=R5 (destination), R2=wire length minus 16-byte tag.
- `+0x220`: call verify with destination R5+bytes-written and tag at
  R5+wire-length-16. Both API returns are byte counts, not replacement pointers.
- `+0x228`: nonzero verify error branches to `+0x278`, then payload cleanup.
- `+0x238..0x256`: on success, update payload length and increment the session
  nonce; the verify-error path skips this increment.
- `+0x436`: call `ScreenStreamProcessData` with R1=R5.
- `+0x44a`: free R5 on the common cleanup path.

The current ELF confirms that allocation/free calls resolve to
`carbox_video_handover_source_malloc` / `carbox_video_handover_producer_free`,
and decrypt/verify still lead to the local ChaCha implementation/wrapper.

Receiver object SHA-256 (identical in the workspace and `~/lib_CarPlay.a`):
`9b051d06928cfb1c7e861ebfca52fd4e902cf0542353c64430c88cf6db1d2126`.

## Consumer and ownership

The link map identifies **ScreenUtilsStub.o** as the implementation of
`ScreenStreamProcessData` (the archive's ScreenUtils.o is empty). Its normal
four-byte NAL-length path edits NAL prefixes in the supplied writable buffer
and calls `DecoderRender(pointer, length)` at `+0x66`. It does not compare the
pointer with the original RX allocation. `DecoderRender` in AppStub forwards
the supplied pointer/length through the synchronous application callback.

ScreenUtilsStub.o SHA-256 (also identical in the customer archive):
`64dd1ef290c6bfc9024611adacf01cd3d4e72389dbc1de62afe1f567f22e7a67`.

The local zero-copy handover tracks allocations by exact pointer and producer
task. `carbox_video_handover_begin` must find the supplied plaintext pointer
in that table; queue publication retains producer and consumer references.
A separate plaintext buffer must be registered and released under this
contract to retain the current zero-copy handover. Ciphertext must have its
own lifetime and release. Merely swapping a pointer in a crypto wrapper is
insufficient.

## Local crypto changes needed for a direct separate-buffer path

`ChaCha20Poly1305.h` already exposes separate `src` and `dst`. The current
mode-2 `chacha20_poly1305_decrypt` stages ciphertext through
`chacha_deferred_copy`; verify subsequently submits `output_base` as both
hardware input and output. Thus changing the receiver arguments alone does
not create a direct source-to-destination DMA path without staging copy.

The managed screen single-pass optimization also currently requires
`ciphertext == plaintext`. Keeping its one rounded ChaCha submission with
separate buffers requires auditing both buffers' alignment/padding/lifetime,
preserving the original wire tag, and adapting the managed-buffer check.

An intended flow can therefore be:

1. Receive into ciphertext A; allocate/register writable plaintext B.
2. Authenticate A and decrypt A to B without staging A into B first.
3. On hardware failure, quiesce DMA and retry from intact A using the same
   key, nonce, AAD and tag; release B to consumers only after verification.
4. Deliver B to ScreenStreamProcessData and the existing queue ownership path;
   release A independently and B after all owners finish.

The receiver call site is in a closed archive. Implementing this requires a
customer receiver change or an explicitly audited adapter for the crypto,
delivery and cleanup boundaries. It is feasible without copying plaintext
back to A, but not a destination-only HAL change. Recovering the same record
before returning a verify error also preserves the caller's normal single
nonce increment. This review does not prove the cause of the observed freeze
or establish runtime performance.
