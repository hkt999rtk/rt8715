# 200 OK to USB timestamp trace

`CAR_ACK_TIMESTAMP=0` is the release default. Set it to `1` to enable `[ACKTS]`
records and the customer HID handler's `hid/end` prints. USB main remains priority 10.
The first HID response creates a priority-2 reporting task. Subsequent transport
hooks only capture timestamps and bounded records; this profiler never prints
from the response, NCM builder, USB owner, or HAL paths. Initial task creation
is included in the first response's measured latency.

| Field | Meaning |
| --- | --- |
| `id` | HID response identifier |
| `pkt` | Trace identifier for one transmitted TCP packet, including retransmissions |
| `seq`, `bytes` | TCP sequence number and TCP payload length for Bus Hunter correlation |
| `enter` | Entry to the cached 200 OK responder, before header preparation |
| `ncm` | Matching Ethernet packet entering the asynchronous NCM queue from lwIP |
| `hw_begin`, `hw_end` | Before/after the real `usbh_hal_hc_start_transfer` call for that NTB buffer |
| `done` | USB owner returning from `carbox_ncm_tx_send_prebuilt` |
| `valid_hw/done` | Whether those stages were observed; zero timestamps with a false flag are missing, not zero latency |
| `delta_us enter_ncm/ncm_hw/hal` | Corresponding intervals from the HAL microsecond clock; hardware intervals require `valid_hw=1` |
| `submit_n` | HAL attempts for this packet; hardware timestamps describe the first attempt |
| `status_hw/tx` | HAL return status and USB owner's send result; interpret only when their valid flags are set |

Absolute timestamps use `clock_gettime(CLOCK_MONOTONIC)` and are printed as
`seconds.milliseconds`. This SDK derives that clock from the 1 kHz RTOS tick.
The additional `delta_us` values use `hal_read_curtime_us()`; they are a separate
clock and must not be subtracted from the monotonic timestamps. Neither clock
is synchronized with Bus Hunter.

The HAL timestamps bracket software programming of the USB controller, not
the instant a USB DATA packet appears on the wire. PING/NAK handling, controller
scheduling, and retries can follow. `done` is the software completion handoff,
not a hardware interrupt timestamp or peer TCP acknowledgement. Failed or
timed-out records remain explicitly identifiable.

The exact sequence range accepted by `tcp_write` is associated with the response
task while lwIP holds its core lock. Matching uses source/destination addresses,
ports, and sequence overlap, so delayed output and retransmissions can match
after the HTTP write returns. A response can produce multiple packet records.
Packets spanning multiple responses are counted as ambiguous rather than
assigned to the wrong response. Pure TCP ACKs are excluded. IPv4 and plain IPv6
are supported; VLANs, IP fragments and IPv6 extension headers are not tracked.

There are 64 response slots and 64 packet slots, with a 2-second correlation /
incomplete-record timeout. Overflow drops diagnostics, never network traffic.
`trace_drop`, `ambiguous_packet`, and `response_overrun` expose these limitations.
An error or a response without a packet match produces a separate record with
`write_ret` and `status`. Status `-2` denotes the void-returning vendor fallback,
whose actual send status is unavailable.

This probe requires the existing single-frame asynchronous NCM pipeline and
cached responder. Build with `CAR_ACK_TIMESTAMP=0` to remove the new probe.

## Additional deferred records

These are per-event records drained by the same priority-2 task every 100 ms,
not 10-second averages. Printed timestamps are capture times, not print times.

| Prefix | Captured information |
| --- | --- |
| `ACKAPP` | Successful `HTTPMessageReadMessage` return in TCPClient, cached responder entry, HID forwarding begin/end; `http_enter_us` includes the vendor `hid` print |
| `ACKQ` | NCM input/ready queue depths, free slots and USB busy before enqueue; first USB main notification/dispatch and maximum observed queue wait for this packet |
| `ACKRX` | Incoming payload on the response TCP connection: TCP sequence/ACK, bytes, lwIP ingress, and USB DONE notification/dispatch when association is proven |
| `ACKUSB` | Driver recovery calls: channel, observed URB state, elapsed ticks, timer base, packet ID when available, and repeated-call count |

Queue depths are separate snapshots, not an atomic view of all queues. The
USB core sidecar follows its FIFO without changing payloads or ordering. A
failed queue send adds no sidecar entry; a mismatch invalidates correlation
until the queue drains. `core_desync` and `core_send_fail` expose these cases.
This requires `USB_CH4_QUEUE_FRONT=0`.

RX tracking starts once the first response establishes the TCP tuple. It logs
payload only, not pure ACKs. TCP ACK numbers are cumulative and do not identify
an HTTP request. `valid_usb=1` requires the same RX submission generation and
the USB main task processing its tracked DONE event; asynchronous ingress or
an earlier queued event processing completion instead yields `valid_usb=0`.
`done_notify` is a software queue notification, not the hardware IRQ time.

For this customer driver, NCM TX uses channel 3 and RX channel 4. URB states
are 0=IDLE, 1=DONE, 2=NOTREADY, 3=ERROR, 4=STALL. Recovery actions are
`nak_check`, `rexfer`, and `nak_irq_enable`. The observed tick value is logged
before the real recovery operation; 4294967295 means no associated tick sample.
The validated driver reaches TX NOTREADY `nak_check` after 80 ticks, RX
NOTREADY after 40, RX IDLE `rexfer` after 80, and TX IDLE `rexfer` after 400.
At high speed 80 ticks are 10 ms. A recovery record is evidence of that code
path, not proof that it caused the full Bus Hunter delay. Repeated identical
channel/state/action/base/packet observations share the first timestamp and
tick sample, plus `count`; ordinary polling is not printed.

There are 64 RX slots and 16 recovery slots. `rx_drop`, `recovery_drop`, and
`response_overrun` expose lost diagnostics under load. All new printing stays
in the reporter; when enabled, the customer HID timing prints are synchronous.

## Customer diagnostic archive

The installed `GCC-RELEASE/carplay_app/lib_Accessory2.a` is the user-supplied
`~/lib_Accessory2.a`, SHA-256:

```text
907dae9ab88d68c8d6f4cc9f96f52b29afd3e807dd7b2ab57d45995dbdf959b4
```

With `CAR_ACK_TIMESTAMP=1`, the derived archive preserves `hid %dms` before
the response and `end %dms` after HID forwarding. Those prints execute on the
event task and can affect timing; `end` does not mean USB completion.
The release default removes those two printf calls and their GetTickCount calls
using validated Thumb NOPs and disabled relocations in the derived archive.
The supplied vendor archive is unchanged. This removal uses the enabled
`CAR_ACK_RESPONSE_CACHE` archive patch. The response patch supports both validated handler
layouts (old 0x1a / call +0x0a, diagnostic 0x3c / call +0x16), and the read-ahead
patch remains applied.

Compared with the previous archive, `AirPlayEvent.o` and `AirPlayResponse.o`
changed. In the latter, the HID forwarding branch now calls `input_hid_report`
before looking up and invoking `acc_carplay_cb_hid_report`. That behavior is
preserved and should be considered when comparing test results.

## Host validation

Run from the repository root:

```sh
python3 project/realtek_amebapro_v0_example/src/carbox/tests/car_ack_timestamp/test_trace.py
```

The harness compiles the real profiler with mocked clock, RTOS, TCP and USB
interfaces and ASan/UBSan. It covers delayed output after write return,
sequence wrap, IPv6 and chained pbufs, peer/task isolation, pure ACK rejection,
failed writes, HAL direction and retry handling, ambiguous coalescing, bounded
overflow, and a reporter clock snapshot preceding a newly created record.
It also covers HTTP/HID timestamps, USB queue wait/full/resynchronization,
RX generation and peer validation, and recovery tick association/coalescing.
Firmware validation checks the final ELF callsites for TCP, HAL, HTTP/HID,
USB queue and recovery wrappers, and the Ethernet ingress hook.
