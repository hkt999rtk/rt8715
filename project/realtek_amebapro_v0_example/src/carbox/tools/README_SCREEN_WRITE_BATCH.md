# Audio precedence and screen write batches

The normal video sender now submits at most 4096 bytes per call to
`carbox_screen_tcp_write`. This includes the existing frame header/ciphertext
stream; no encryption, frame format, or byte ordering changes. The customer's
loop advances its pointer and remaining length by the actual return value and
retains its native 250 ms EAGAIN poll. No additional delay or yield is inserted.

The exact task name `AudioOutput` is assigned priority 5 at xTaskCreate, for
both music and navigation. ScreenThread remains at its requested priority 4;
the decoder remains 6. A waiting audio writer can therefore run at a screen
core-lock release boundary. This is not a bound on lwIP critical-section wall
time: tcp_output can send already-queued segments, and higher priority work
can preempt a lock owner.

## Build settings

- `AUDIO_OUTPUT_TASK_PRIORITY=5`; `-1` preserves the library's request.
- `SCREEN_TX_WRITE_CHUNK_BYTES=4096`; must be positive.
- `SCREEN_TX_BATCH_DIAG=0` by default; set to 1 for statistics. Both behavior changes remain active when it is 0.

These settings have a dedicated build stamp covering all affected objects.
The reporter remains linkable when the poll opt-in switch is disabled.
The normal-frame relocation to `carbox_screen_tcp_write` must be present
(`SCREEN_TX_POLL_WAIT=1` in the current firmware). Other socket writers are
not capped by this helper.

## Log interpretation

`[SCREEN_WRITE]` comes from the existing priority-1 `wlan_rx_diag` task, at
least five seconds apart (normally about six seconds with its two-second loop).
The write path only records counters and timestamps; it does not print, wait,
or allocate. The shared nonblocking UART logger can skip a report on contention.

| Field | Meaning |
| --- | --- |
| `tick`, `window_ms` | Snapshot time and interval since the preceding snapshot |
| `cap` | Configured per-call submission limit, 4096 |
| `writes` | Completed write attempts in this interval, including failed attempts |
| `total_writes` | Cumulative attempts modulo 2^32, retained across skipped log output |
| `limited` | Attempts whose caller requested more than the cap |
| `submitted` | Sum of capped requested bytes, including retries; not network traffic |
| `accepted` | Sum of positive write return values; not an ACK/delivery count |
| `submit_max` | Largest single submission in this interval, at most 4096 |
| `partial` | Positive return smaller than the capped submission; deliberate capping is excluded |
| `eagain`, `errors` | Would-block attempts and other failed attempts respectively |
| `zero` | Zero returns on nonempty submissions; zero-length calls are excluded |
| `write_max_us` | Largest helper elapsed time, including USB route opt-in and scheduling delays |

All fields except total_writes are interval statistics. No per-frame attribution
is claimed. Existing NAV_TX core-wait and send-gap diagnostics remain necessary
to assess whether the audio latency tail improves. Hardware testing should also
check video throughput and EAGAIN frequency because smaller writes increase
socket-call overhead and may alter TCP segmentation.

## Verification

Run `python3 test_screen_write_batch.py`, `python3 test_audio_output_priority.py`,
and `python3 -m unittest test_screen_tx_poll_patch` from this directory. The
batch harness executes the production helper with diagnostics on/off and checks
byte-for-byte stream preservation across partial writes and EAGAIN, fatal/zero
returns, and deferred statistics. The archive tests validate native poll and
call relocation compatibility. The harness also simulates the shared lwIP error
being overwritten during instrumentation; a failure's captured error is restored
just before returning. This narrows the added exposure, but does not eliminate
the platform's pre-existing global errno race. Verify the final ELF helper call
and priority override after building with the Realtek toolchain.
