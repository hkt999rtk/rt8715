# TCP HID response latency regression

The local lwIP nonblocking receive implementation retried an empty mailbox
with a 10 ms blocking fetch. This also affected sockets.c's DONTBLOCK probe
after it had already collected payload bytes, delaying delivery to TCPClient.
Remove that retry: an empty nonblocking receive returns ERR_WOULDBLOCK, while
blocking receive keeps its existing timeout. Data arriving later is available
to the next read.

Board testing confirmed that this change resolves the reported fixed HTTP
200 OK delay. Before the fix, the captured lwIP ingress to HTTP parse completion
interval was about 10–11 ms; response entry to USB HAL submission was below
1 ms in the supplied samples.

Run the host regression from any directory:

```sh
python3 project/realtek_amebapro_v0_example/src/carbox/tests/nonblocking_recv/test_recv.py
```

The harness extracts the actual receive function and compiles it with mailbox
mocks and ASan/UBSan, both with and without LWIP_SO_RCVTIMEO. It checks empty
nonblocking reads, the partial-read follow-up, late arrivals, errors/closure,
and unchanged blocking timeouts. The original source fails the no-wait check.

Release defaults retain USB main priority 10 and the single-frame NCM TX
pipeline used during board validation. CAR_ACK_TIMESTAMP defaults to 0:
ACKTS/ACKAPP/ACKRX/ACKQ/ACKUSB instrumentation is compiled out, and the enabled
cached-response archive patch removes the customer HID handler's hid/end
printf and GetTickCount calls. Set CAR_ACK_TIMESTAMP=1 to restore diagnostics;
configuration stamps rebuild affected objects and the derived vendor archive.
