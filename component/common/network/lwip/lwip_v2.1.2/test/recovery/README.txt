TCP recovery packet-injection regression tests
=============================================
Run ./run.sh from this directory (or via its repository path).
It compiles the production lwIP core on the host with a simulated network
interface and clock; no device or network access is needed.

Memory/undefined-behaviour checks:
  TEST_CFLAGS='-fsanitize=undefined,address -fno-omit-frame-pointer' ./run.sh
Legacy feature-disabled smoke tests:
  TEST_CFLAGS=-DLWIP_TCP_RECOVERY=0 ./run.sh handshake_idle timer_cadence zero_window

Coverage: first/second duplicate ACK Limited Transmit, multiple losses,
partial and full ACKs, retained driver pbufs before fast/partial/output,
output rejection/retry, peer window constraints/reopening, reordering,
recovery boundary, many successful partial repairs vs retry budget,
32-bit sequence/time wrap, moving clock during deferred recovery, Karn
exclusion, handshake/idle sampling, RTO backoff and maintenance cadence.
The four original limited/partial/rto/busy tests fail against legacy code.
The tests exercise TCP, not real USB scheduling or the peer NCM parser.

Implementation
--------------
LWIP_TCP_RECOVERY=1 in platform lwipopts.h enables all three improvements.
The generic lwIP default remains disabled. New state is appended to tcp_pcb
so existing member offsets stay unchanged. Rebuild all source users including
memp.c whenever changing this setting; do not mix PCB sizes across objects.
SYN timers and persist/keepalive/TIME_WAIT keep their existing cadence.
Established/closing TCP checks deadlines every 10ms, default initial RTO 3000ms,
minimum 1000ms, maximum 60000ms; backoff is retained until a valid RTT sample.
Explicit USB screen opt-in uses initial/minimum RTO 50ms (details below).
RTT samples do not include retransmitted flight. No SACK sender is added.

Release defaults
----------------
TCP recovery and screen POLLOUT waits remain enabled. Screen/TCP, audio, RX,
CPU and heap diagnostic reports default off, including TCP_RECOVERY_TRACE and
SCREEN_WAKE_TRACE. Release builds omit their trace rings and reporting hooks. In the derived screen archive,
only the normal-frame EAGAIN puts call is redirected to a quiet helper; other
errors and partial-write retry semantics are preserved. The original customer
archives remain the patch inputs. Use a clean build when switching profiles.

Observation
-----------
Build SCREEN_BLOCK_PROFILE=1 SCREEN_TCP_ACK_PROFILE=1 with the existing
PC_PROFILER=1 / PC_PROFILER_SELF_REPORT=1 statistics thread enabled.
[TCPREC][CONFIG] confirms the linked configuration once.
[TCPREC][n] counters are deltas since the previous report, across ALL TCP
connections. last_local/remote and last_event identify ONLY the last event;
they do not assign all interval counters to that connection.
last_event/kind_ms = numeric event code / sys_now() timestamp in milliseconds:
  1 Limited Transmit, 2 fast recovery entry, 3 partial ACK, 4 full recovery,
  5 busy buffer retry, 6 RTO, 7 output error.
recovery_max_ms is the longest completed fast recovery in this interval.
deadline_late_max_ms includes scheduling delay and any busy-buffer deferral.
A pending recovery spanning intervals is counted on completion; no event
line is printed from packet/interrupt context. Counters use static storage;
the statistics thread snapshots them under a short critical section.
The RTT line describes sampled RTTs, and srtt/rto_ms the latest RTT update.
event_rto_ms belongs to the last event (after backoff for event 6).

For customer comparisons retain SCREENFRAME frame_id, SCREENACK, NCM and
CPU statistics alongside TCPREC. A shorter local recovery does not prove
where the original missing packet was dropped; confirm with a new capture.

References:
https://www.rfc-editor.org/rfc/rfc3042.html
https://www.rfc-editor.org/rfc/rfc6582.html
https://www.rfc-editor.org/rfc/rfc6298.html

Per-event evidence (2026-11-02)
-------------------------------
Opt-in test firmware command (after clean and forced sequential ram_lp):
  make -j8 ram_is CROSS_COMPILE=<Realtek prefix> TCP_RECOVERY_TRACE=1 \
    PC_PROFILER=1 PC_PROFILER_SELF_REPORT=1 SCREEN_BLOCK_PROFILE=1 \
    SCREEN_TCP_ACK_PROFILE=1
Use PYTHONPATH=/tmp/rt8715-python-deps if image packing needs those packages.
All release defaults stay off. TCP_RECOVERY_TRACE has a dependency stamp for
all source consumers because its PCB tail grows. Existing PCB offsets stay
unchanged. The source lwIP memp allocator must be rebuilt with the same flag.

No UART, allocation or list scans occur in the TCP trace hooks. Each ACK/TX
updates only per-PCB scalar fields. Interesting events copy one fixed record
into a 32-slot normal ring or a separate 32-slot RTO ring. Reporter copies one
record under SYS_ARCH_PROTECT, unlocks, then prints from the existing statistics
task. At most 32 records per class per report, yielding one tick per record.
This has bounded nonzero CPU/UART cost; measure timing effects on hardware.
Each ring overwrites its own oldest entry when full. overwritten_total is
cumulative and means trace loss, never network packet loss. Normal recovery
traffic cannot evict the RTO ring. Only the first partial ACK per recovery is
emitted; partial_count at full/RTO describes all observed partial ACKs.

TCPREC_EVT id joins TCPREC_SEQ/WIN/IO lines. conn is a monotonic per-PCB-lifetime
identity, independent of reused ports/addresses. Ports are local/remote.
RTO records print before normal records: sort by recorded ms/id, not UART order.
Timestamps are wrapping sys_now() milliseconds; sequence numbers are raw TCP
numbers for matching packet captures (not Wireshark relative numbers).

Event semantics:
  fast: after entering fast recovery; full: after leaving it.
  partial_first: first partial ACK after its next-hole requeue attempt.
  fast_skip: third-duplicate-ACK fast recovery rejected; flags/recover/valid
    distinguish an active recovery/RTO from the recover_end guard.
  rto_due: BEFORE queue changes/backoff, original timer age and RTO preserved.
    A busy retry can produce another due event without a committed retransmit.
  rto_arm: after successful preparation and backoff, BEFORE output commit.
    nrtx is still the pre-commit count; flags/cwnd/RTO reflect this point.
  rto_tx: first successful tcp_output segment submission after that arm.
  rto_ack: first ACK that advances lastack after arm. It may acknowledge an
    original delayed packet; it is not proof the retransmission caused it.
  retx: other successful submissions with sequence below snd_nxt.
  output_error/busy: rejected output or buffer preventing recovery.
  ack_gap: >=100ms between ACK arrivals with outstanding data; not idle gaps.

IO ack_no/wnd is the last incoming ACK observed inside tcp_receive, including
old/future ACK values; progress_age records actual accepted ACK advancement.
ack_age is time since any observed ACK; gap is the interval preceding that ACK.
wnd is scaled advertised capacity; TCPREC_WIN wnd is the accepted pcb window.
tx_seq/end is the last successful segment submission, exclusive end. This is
NOT USB completion or peer receipt. RTO_TX captures the retransmitted segment
before snd_nxt advancement. heads bits 1/2 mean unacked/unsent head exists;
head triples are seq/TCP length/pbuf ref. pending bits 1/2/4 are fast/partial/
output retries. recover_valid qualifies recover_end. Age 4294967295 means
unobserved/disabled timer. No diagnostic snapshot dereferences a PCB later.

Tests:
  TEST_CFLAGS=-DCONFIG_TCP_RECOVERY_TRACE=1 ./run.sh
  TEST_CFLAGS=-DCONFIG_TCP_RECOVERY_TRACE=1 ./run.sh trace_evidence trace_fast trace_wrap
These exercise original RTO vs backoff, actual retry and ACK timing, connection
reuse, independent ring overflow, first partial ACK sampling and clock wrap.

Frozen pre-RTO evidence (2026-11-02)
-----------------------------------
With CONFIG_TCP_RECOVERY_TRACE=1, TCPCAUSE supplements TCPREC and TCPWAKE.
The latest FULL snapshot per connection anchors a bounded eight-entry history.
After FULL, ACK packets reaching tcp_receive, advancing-ACK calls/bytes and
last advancing ACK time/sequence are counted; FAST_SKIP saves its latest full
snapshot, count and OR of reason bits. The FULL-causing ACK is excluded from
these interval counters. Advancing bytes are sequence progress, not playback.
Only FAST_SKIP calls with dupacks==3 count, matching the existing trace hook;
this is NOT a count of all duplicate ACKs or all blocked output attempts.
Reason bits (hex): 1 no unacked, 2 already in fast recovery, 4 in RTO recovery,
8 lastack <= recover_end while recover_valid. Multiple bits can coexist.
No recovery condition, window, retransmission timer or send behavior changes.

At successful RTO prepare/ARM, the pre-prepare DUE snapshot and its connection's
history are copied into an independent eight-entry frozen FIFO. Failed prepare
(busy buffer) attempts do not fill this FIFO. Ordinary event-ring overflow
cannot change frozen records. FIFO full drops NEW captures, preserving the
oldest evidence. TCPCAUSE_QUEUE dropped_new_total reports this explicitly.
History replacement is round-robin among connections that have reached FULL;
history_evicted_total reports replacements (including old closed connections).
Connection generations prevent stale history from attaching to a reused PCB.
full_id=0 means no retained FULL anchor, not proof that recovery never occurred;
then skip/ACK history fields are unavailable. skip_id=0 means no retained skip
since that FULL. progress=0 means last_progress_ms/ack are unavailable.
Consecutive RTO snapshots can share the same FULL anchor and cumulative counts.

All TCPCAUSE lines join by rto_id (the original TCPREC rto_due event id).
FULL/SKIP/DUE snapshots include original ms; full_age_ms uses unsigned elapsed
arithmetic. Unknown age is 4294967295. A frozen FULL sndbuf is sampled before
that ACK's snd_buf credit, as with existing TCPREC_FULL: use TCPWAKE for actual
freed bytes and readiness. TX is TCP output acceptance, not wire completion.
General events are still lossy; the frozen record is a summary, not a packet
capture. Neither this nor TCPWAKE proves where an original packet/ACK was lost.

Hooks allocate nothing, traverse no packet lists, and never print. ACK updates
perform at most eight connection comparisons and scalar counter writes. The
FIFO performs a bounded record copy under SYS_ARCH_PROTECT. The existing
statistics task prints at most eight captures per cycle, yielding per capture.
Printing still adds bounded UART/task load; performance impact requires board
measurement, not a claim of zero overhead. No new task is created.

Checks:
 TEST_CFLAGS=-DCONFIG_TCP_RECOVERY_TRACE=1 ./run.sh trace_cause cause_wrap
These inject real ACKs and cover boundary skip, no-progress and advancing ACKs,
normal-ring flooding, frozen record immutability, PCB reuse/no FULL anchor,
frozen FIFO overflow/drop-new behavior, and sys_now wrap.

ACK heuristic and RTO handover (2026-11-02)
------------------------------------------
Implements RFC 6582 section 4.1 under LWIP_TCP_RECOVERY. The old unconditional
TF_RTO / lastack<=recover_end exclusions now admit fast recovery when the
latest forward ACK advanced at most four MSS, cwnd is greater than one MSS,
the peer window is nonzero, and state is ESTABLISHED. No forward ACK since
RTO means ineligible. A third duplicate ACK is still required by TCP input;
already-active fast recovery is never reentered. Sequence comparisons keep
exclusive recover_end semantics; this is not a <= to < boundary change.

The entry commits only after tcp_rexmit successfully queues the lowest
unacked segment. A driver-held pbuf leaves flags, congestion state and timer
unchanged and uses the existing deferred retry. Successful entry clears
TF_RTO, sets the new recovery boundary, then uses normal NewReno partial/full
ACK handling. An output error keeps the queued retransmission for retry.
RTO minimum/backoff/Karn policy are unchanged. RTO clears ACK heuristic
eligibility; ACK progress refreshes it. There is no extra timer or task.

This is a loss heuristic, not proof of packet loss. Reordering and duplicated
ACKs may cause an unnecessary retransmission and congestion-window change.
The >4 MSS check retains protection against many ACKs caused by unnecessary
RTO retransmissions. No-ACK stalls and ineligible jumps still rely on RTO.

Additional tests: heuristic_rto, heuristic_tail (237-byte old-flight tail),
heuristic_busy, heuristic_output, heuristic_guards, heuristic_timeout,
heuristic_wrap (sequence AND clock wrap), and trace-only heuristic_evidence.
The new RTO entry test was observed failing on the previous implementation.
Run all default tests and, with CONFIG_TCP_RECOVERY_TRACE=1, additionally:
  ./run.sh trace_fast trace_evidence trace_wrap trace_cause cause_wrap heuristic_evidence

Observations use the existing statistics task, never printf on TCP input:
[TCPREC][CONFIG] ack_heuristic=1 confirms this policy.
[TCPHEUR][n] counts deltas across all TCP connections: enter, from_rto,
reject (at third dupack only), first_progress, rto_before_progress and the
maximum time from successful requeue to the first advancing ACK. Completion
may occur in a later reporting interval than entry. Close before either ACK
or RTO is not counted as either outcome. These are correlation, not proof
that the retransmission caused that ACK, nor proof of full recovery.

Per-event ack_heuristic captures OLD flags/cwnd/recover_end after successful
requeue, before transition; heuristic_ack captures the first forward ACK.
Both use the urgent ring, with bounded storage and overwritten_total.
[TCPHEUR_EVT] supplies latest ack_advance, rto_end, heuristic rejection bits,
and time since entry (4294967295 means no pending correlation). Reject bits:
1 no post-RTO progress; 2 cwnd<=MSS; 4 ACK jump>4*MSS; 8 not ESTABLISHED;
16 peer window zero. A zero mask means the heuristic gate alone allows entry;
normal no-unacked / already-in-recovery exclusions still apply. Old
skip_reason bits describe active guards, not a new unconditional prohibition.
[TCPCAUSE_HEUR] freezes these fields at last skip and RTO due, surviving
ordinary trace-ring eviction. skip_id=0 still means no retained skip.

For board validation compare screen frame durations, RTO count, first ACK
progress, repeated heuristic entries, queue drops and CPU load against the
2 KiB TCP_SNDLOWAT-only image. Keep 250 ms poll and the 2 KiB low-water value
identical between the images. Firmware now adds 12 bytes of non-trace PCB
state; rebuild all lwIP source consumers, including memp.c and wrappers.

USB screen 50ms RTO experiment (2026-11-02)
-----------------------------------------
User requested aggressive recovery on the USB interface. Only established
screen senders explicitly calling lwip_usb_screen_rto_enable(fd), whose output
route is the USB NCM "en" netif, opt in. No port numbers, task handles, or stale
fd-to-PCB pointers are used. A new/reused PCB starts with the default policy.

The production normal-frame write hook enables it before the first write in
both quiet and diagnostic builds. The archive patch redirects only the
validated write call; other callers and partial-write/error results are
unchanged. The screen poll-wait hook also enables it at EAGAIN before sleeping.
There is no dependency on the profiling wrapper or its reporting settings.
A customer calling their own poll implementation can call the same socket API
once after connection establishment, before the first screen write.

Initial RTO without an RTT sample is 50ms. With samples, RTO is
max(50ms, SRTT + max(10ms, 4*RTTVAR)), capped at 60s. Repeated poll calls never
reset the deadline or exponential backoff. Existing RTO/Karn recovery retains
its current timeout until a clean RTT sample. Other TCP retains the 1s floor
and 3s initial timeout; SYN, persist, maintenance and retry limits are unchanged.
The policy assumes this socket keeps its USB route for its lifetime.

10ms checks mean a 50ms deadline normally fires at 50..60ms plus scheduling
latency, not a hard real-time guarantee. This is a deliberate experiment below
the RFC 6298 recommended 1s minimum: delayed ACKs or peer stalls may cause
spurious retransmissions and cwnd reduction. Keep trace/USB/CPU statistics on
for customer testing; compare frame stalls, RTO/fast counts and goodput.
More frequent checks also increase TCP timer wakeups from 20 to 100 per second.
No per-packet printf or allocations were added to TCP recovery.

Tests usb_screen_rto/scope/jitter/initial/wrap exercise actual injected ACKs,
50ms deadlines, exponential backoff, Karn, new-PCB isolation, non-USB and
unroutable/handshake refusal, warm opt-in, RTT growth, idle behavior, 32-bit
clock wrap, and no backoff reset on repeated or late opt-in.
