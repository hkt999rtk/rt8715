# RX software recovery after hardware failure

## Review result (2026-09-20)

The customer log contains hardware completion timeouts followed by nine
same-record software authentication failures. This change does not establish
their root cause and does not change the hardware timeout or socket handling.

The reviewed mode-2 lifecycle is:

- Software init derives the Poly1305 key and leaves the ChaCha block counter
  at 1. The alias wrapper snapshots overlapping caller key/nonce before init
  and restores persistent material only after verify/final.
- AAD updates feed the software accumulator and retain a separate AAD copy.
- Generic decrypt stages ciphertext and advances the logical byte count, not
  the software cipher or Poly1305 payload accumulator. Direct RX registers an
  independent source and destination without processing the payload.
- Generic verify saves the wire tag and copies staged ciphertext to a private
  hardware source. Direct RX retains its independent source and saves the tag.
- The previous HW-error fallback reused the software accumulator. Under these
  invariants that is valid; review found no unconditional violation explaining
  the customer failures. Source/destination separation already protects against
  ordinary partial DMA writes to the output.

## Change

After a quiesced RX hardware operation failure, both staged and direct paths
call `chacha_rx_retry_fresh()`:

1. Initialize a fresh software state from key/nonce exported before hardware
   submission and the retained AAD snapshot. Authenticate the retained source
   against the saved tag using the same record nonce.
2. Run the reused-state fallback as a diagnostic comparison. Skip this
   comparison if its Poly1305 buffer count or padding flag is out of bounds.
3. If fresh authentication passes, generate the authoritative plaintext with
   the fresh state. Compare it byte-for-byte with reused-state output when the
   latter authenticated. A counter-only fault may leave tag verification valid
   while producing incorrect plaintext, so comparing tags alone is insufficient.
4. If fresh authentication fails, clear output and return an error, including
   a contradictory reused-pass/fresh-fail result. Do not bypass authentication.

Successful hardware operations and no-hardware eligibility fallbacks are
unchanged. TX is unchanged. No extra heap allocation, source hash, or full-record
copy is added to successful HW operations. Failed operations pay for the two
software paths; the helper uses one local state and 64-byte comparison scratch.
Its local state and scratch are wiped regardless of `CHACHA_ENABLE_CLEAR`.

The existing next-record nonce diagnostic is unchanged; this new retry itself
never guesses or changes the caller's record nonce. It uses the existing source
ownership contract, not a second independent pre-DMA copy of that source. Thus
it does not prove source/AAD contents remained unchanged during hardware work.

## Failure-only log

`[CHACHASWREC][RX]` has the same recovery ID as `[CHACHAREC][RX]`:

| Field | Meaning |
| --- | --- |
| `path` | `staged` legacy streaming path or `direct` separate-buffer adapter |
| `reuse_ran` | Old accumulator comparison was safe to run |
| `reuse_tag_ok` | Old accumulator accepted the saved tag |
| `fresh_tag_ok` | Fresh accumulator accepted that tag |
| `plain_match` | 1 = exact match; 0 = different plaintext; -1 = not compared because both paths did not authenticate |
| `counter_ok` | Old ChaCha block counter was 1 before retry; **not** the session record nonce |
| `aad_len_ok` | Old accumulator AAD length matched retained AAD length |
| `aad_padded`, `poly_leftover` | Old state diagnostics captured before retry |
| `selected` | `fresh` authenticated output, or `reject` |

Interpretation:

- Reuse fails / fresh passes: investigate reused state, AAD accumulation and
  state lifetime; this is evidence against a failure of the fresh input tuple.
- Both pass / plaintext differs: investigate cipher key/counter/nonce state.
- Both fail: no recovery; investigate source/AAD/tag/key/nonce/length inputs,
  record synchronization and shared software implementation. This alone does
  not identify which input is wrong.
- Both pass / plaintext matches: the hardware failure was recovered normally.

Only metadata and comparison results are printed. No key, nonce, wire tag or
plaintext dump is needed for this comparison. Normal profiling remains off.

## Validation

Run from this directory:

```sh
make host-tx-rx-recovery-check
make host-rx-recovery-check
make host-check
```

The recovery tests use independent OpenSSL vectors and a mocked hardware
backend, with AddressSanitizer and UndefinedBehaviorSanitizer. They cover
partial writes, tag rejection, allocation failures, separate/in-place callers,
streaming updates, adapters, alias restoration, and subsequent hardware use.

Added cases inject corruption into the reused Poly1305 accumulator, cipher
counter, AAD length, cipher key and Poly1305 leftover count at the simulated
hardware error. Both direct and staged RX are tested with valid/bad tags,
including the nine customer failure lengths (2042, 6132, 8673, 1795, 8597, 9197,
2005, 2756, 5613), a combined-backend length and a chunked-backend length.
The log checker asserts fresh/reuse/plaintext outcomes, not just program exit.

These are injected fault models, not a reproduction of the customer's hardware
fault. All three targets passed; TX/RX recovery matched 298 failures/retries.
The TX/RX target also passed with `CHACHA_ENABLE_CLEAR=1` and `-Werror`.
Incremental Realtek `ram_is` build/link passed after verifying the LP ELF exists.
Board testing is still required for DMA/cache/IRQ timing, stack headroom and
the real long-duration failure. Capture the new failure-only log together with
the original recovery and subsequent-frame logs.
