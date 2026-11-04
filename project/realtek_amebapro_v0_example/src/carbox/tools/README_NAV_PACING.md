# Navigation output pacing test

Release defaults enable `AUDIO_NAV_PACING=1 AUDIO_MUSIC_DIAG=1`.
Set `AUDIO_DIAG_LOG=1` to print the statistics below; default is 0.
The output hooks and epoch housekeeping remain active when quiet. This policy adds no new
prefill, PLC change, sample removal, resampling or RTP timestamp manipulation.
Only type 101 / 48 kHz / stereo 16-bit output uses it; type 100 stays native.
Both native payloads, 1440 and 1416 bytes, are handled (7.5 and 7.375 ms).

The post-send controller uses an 80 ms time-weighted queue filter, 50 ms target,
0–5% boost and maximum 10 percentage points/second slew. Below 15 ms actual ring
content it cancels boost. Fractional microseconds accumulate across deadlines.
Version `mode=bounded-phase-v2` fixes the log15/log16 overflow regression:
a late send no longer adds another full wait. The media phase is separate from
the wake deadline. Up to 40 ms of phase debt is retained and repaid with a minimum
send-entry spacing of 2/3 of nominal (5 ms / 4917 us for 1440 / 1416 bytes).
Thus transient catch-up is capped at 1.5x nominal; normal queue boost stays 0–5%.
Debt beyond 40 ms is rebased, with excess backlog left to the queue controller.
No PCM samples are discarded or packet timestamps changed by this policy.
The throughput cannot be guaranteed if downstream service/scheduling is slower
than the allowed catch-up rate for a sustained period; push loss stays visible.
The native empty polling remains, and a resumed producer restarts the deadline.
No new prefill means the ring is not guaranteed to contain 50 ms at startup.
At >=80% capacity pressure is counted, with the same bounded pace; native push
failures remain observable. There is no new emergency packet/sample discard.

## Statistics

`[NAV_PACE] epoch=N` joins `[MUSIC_OUT] epoch=N`. The reporter runs every 5 seconds
and after close; fields are cumulative within an output epoch. No per-packet print.
All `_us` fields use wrapping hardware microseconds; `tick` is the RTOS clock.

- `enabled=1`: supported format and pacer allocation succeeded. `enabled=0`:
  unsupported format or allocation failure; native scheduling is retained. Slot
  exhaustion remains visible through `MUSIC_OUT_LOSS`.
- `q_us`, `q_filter_us`, `q_min/max_us`: raw last, filtered post-send, cumulative
  observed min/max ring duration. Raw observations include push/pop and startup.
- `boost_ppm`: 10000 is 1%; cap 50000 is 5%. `interval_us` is the latest desired
  interval; fractional carry means adjacent integer values may differ by 1 us.
- `rebases`: phase-debt clamps at 40 ms (v1 counted every overdue deadline).
  `late_max_us`: maximum overdue phase before clamping.
- `debt_us`, `debt_max_us`: current/max retained phase debt, capped at 40 ms.
- `catchup_waits`: times the minimum spacing limited catch-up; not packet loss.
  `min_gap_us`: enforced send-entry spacing floor; service time counts toward it.
- `pressure_samples`: post-send samples at >=80% ring capacity, not dropped packets.
- `entry_gap_avg/max_us`: sendto entry-to-entry spacing (not USB wire timing).
  `return_gap_max_us`: return-to-return spacing. `gap_gt10/20/40ms` are overlapping
  cumulative threshold counts. `gap_seq/ts/at_us` identify the largest entry gap.
- `startup_empty_us`: initial empty episode before first successful pop.
- `empty_resume_n`, `empty_resume_avg/max_us`: empty episodes followed by another
  successful pop. Duration covers first failed pop until successful pop, not poll count.
- `terminal_empty_us`: still-open empty episode when the output thread exits.
- `pending_empty_us`: currently open episode at the snapshot, not yet classified.
  These fields distinguish lifetime stages, not audible speech versus silent PCM.

Keep existing NAV RX/DEC/PCM/CONT and MUSIC_OUT loss counters. `send_ok` means local
stack acceptance; it does not prove USB completion or vehicle jitter-buffer health.
Compare identical navigation/audio scenarios and mark the audible word/time. Watch
whether entry-gap spikes, resumed empties and pressure correlate with the symptom.

## Verification

Host: `test_nav_output_pacing.py`, `test_nav_output_integration.py`,
`test_nav_output_patch.py`, `test_nav_output_abi.py` (Unicorn/pyelftools).
Linked: `verify_audio_music_output.py`, `verify_nav_output_pacing.py` and
`verify_nav_playout.py`. The added native call site is +0x118 in the hash-pinned
AirPlayAudio.o; vendor input is preserved. Assembly returns to native loop +0x90
for handled navigation, otherwise returns RingBuffer_size to native +0x11c.
