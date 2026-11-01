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
Established/closing TCP checks deadlines every 50ms, initial RTO 3000ms,
minimum 1000ms, maximum 60000ms; backoff is retained until a valid RTT sample.
RTT samples do not include retransmitted flight. No SACK sender is added.

Release defaults
----------------
TCP recovery and screen POLLOUT waits remain enabled. Screen/TCP, audio, RX,
CPU and heap diagnostic reports default off. In the derived screen archive,
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
