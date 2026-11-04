# Navigation onset, FDK and send-path evidence

Enable `AUDIO_NAV_TX_TRACE=1` with `AUDIO_MUSIC_DIAG=1`,
`AUDIO_NAV_PACING=1`, `AUDIO_NAV_CONTINUITY=1`, `NCM_TX_ASYNC=1` and
`NCM_TX_PIPELINE=1`. Trace defaults to off. The existing pacing v2 policy,
PCM samples, RTP timestamps and FDK concealment parameters are unchanged.

## Reports

| Log | What it measures |
| --- | --- |
| `NAV_ONSET` | First second of output sends in each navigation output epoch: first queue depth, min/max queue, send-entry gaps, send duration, pacing boost/debt and empty-ring resume count. |
| `NAV_FDK_STATE` | Actual FDK concealment state before/after Apply, input navigation session/RTP timestamp, frame validity and whether our missing-frame path explicitly requested PLC. |
| `NAV_TX` | Cumulative completed/failed/unmatched traces and diagnostic loss/expiry counts; FDK call/non-Ok/transition counts. |
| `NAV_TX_METRIC` | Cumulative counts and average/max elapsed microseconds for the stages below. |
| `NAV_TX_SLOW` | Output epoch, RTP sequence/timestamp, queue/debt, stage-presence mask and timings for a slow or failed send. |

FDK states are `0=Ok`, `1=Single`, `2=FadeIn`, `3=Mute`, `4=FadeOut`.
FDK initializes to Ok. A fresh stream does not necessarily fade in. Source
defaults specify five fade frames, but runtime concealment mode and state
transitions determine which processing actually runs. No gain parameters are
changed by this diagnostic build. Compare state transitions with `NAV_CONT`
loss/PLC statistics; `plc_ok` alone does not describe the gain state.

`NAV_ONSET` starts at the first output send, not the first audible syllable or
the recording start. It closes after one second and does not reopen at the
32-bit microsecond clock wrap. Gap thresholds are descriptive counts
(`gap_lt6/gt10ms`), not proof of a playback fault. Nominal output packets are
7.5 ms or 7.375 ms. Input decoder timestamps and outgoing PCM RTP timestamps
belong to different domains: do not equate them. Use session/epoch lifecycle
and `at_us` to relate events.

## Send-path stages

| Metric ID | Interval |
| --- | --- |
| 0 | Entry to return of `lwip_sendto` |
| 1 | Send entry through pbuf allocation/copy and address preparation |
| 2 | Waiting for the lwIP core mutex |
| 3 | Holding the core mutex |
| 4 | NCM input enqueue attempt to dequeue |
| 5 | Dequeue to the build attempt that includes the frame (gather/free-slot wait) |
| 6 | Ready-queue enqueue attempt to USB HAL call |
| 7 | USB HAL call to its return |

Detailed `build` is build entry to ready-queue enqueue attempt. `end2end` is
send entry to HAL return. These intervals can overlap because NCM is async;
do not add all fields together. `depth` is the queue occupancy sampled before
enqueue, not a high-water mark. Ready-queue timing includes any enqueue wait.

Stage mask bits: 0 entry, 1 prepared, 2 core wait, 3 core acquired, 4 core
release, 5 NCM enqueue, 6 dequeue, 7 build, 8 ready enqueue, 9 USB call,
10 HAL return/terminal transport failure, 11 send return. A zero duration
without both bits is missing evidence. HAL status `-2` denotes input enqueue
failure and `-3` source release without a built NTB. Neither is a USB return.

The core owner is a snapshot at wait entry, not a complete ownership history.
`owner=0` means no tracked owner; scheduling around mutex hooks can create an
unobserved boundary. Owner call sites require this build's ELF to symbolize.
The send path uses core locking, not a tcpip mailbox round trip.

## Actual customer NCM path

With `NCM_TX_VENDOR_ASYNC=1`, the linked code is `ncm_tx_async.o` from
`usb_lib/build/lib_usbsmart.a`, not the local ethernetif worker. The build
creates `lib_usbsmart_link.a` with scoped symbol redirects for diagnostics.
The original archive is untouched. `patch_nav_tx_vendor.py` refuses objects
other than the reviewed SHA256
`678cd07378f07afd6aeb4f0bde31c2d8ac550d8726012ed88f040f9b42f2a964`.

Reviewed ABI: 20-byte queue item, 648-byte batch descriptor, up to 16 frames
and 64 segments. The adapter follows each frame through partial batch builds
and source pbuf release; multiple navigation frames in one NTB retain separate
trace IDs. Queue arguments, waits, scheduling, batch sizes and native returns
are forwarded unchanged. Build flags invalidate the derived archive.

Exact pbuf identity excludes incidental ARP/ND packets. Deferred neighbor
resolution or fragmentation that replaces the pbuf is not correlated and is
counted as `no_ncm`. That count does not prove packet loss. HAL success does
not prove reception or playback by the car head unit.

## Cost and limits

Hot paths store fixed-size metadata and counters, with bounded critical
sections; no new hot-path printing or dynamic allocation is used. Tables live
in LPDDR, outside the nearly full internal SRAM. This still adds measurement
overhead; its device-level timing impact needs testing.

There are 64 outstanding traces, 8 task contexts, 16 slow details and 16 FDK
events. Traces expire after five seconds at the next report. Slow means send
at least 5 ms, enqueue-to-HAL at least 10 ms, or error. The shared reporter
prints at most four slow records and four FDK events per report, no more often
than every five seconds (usually six seconds with its existing two-second
cadence). Summary counters are cumulative. Check all loss/expiry counters
before interpreting absent detail as absence of an issue. Metadata contains
no PCM payload.

## Customer capture

Record the firmware build version, capture serial logs before navigation
starts, mark the navigation phrase and video time of the wobble, and continue
logging for at least 30 seconds afterward so deferred records drain. Record
whether the event is a new stream or a later phrase in an existing stream.
Retain the matching `flash_is.bin`, ELF, map and manifest for time alignment
and owner-site decoding. Successful host tests and link checks do not replace
the customer playback test.
