# CarPlay Video/Audio Media Transport Investigation

## 1. Scope

This report documents how the current project transports CarPlay video and
audio after authentication and session establishment. Pairing, certificate
authentication, and initial iAP2 identification are intentionally excluded.

The analysis is based on:

- active build configuration and local source code;
- symbols and strings from the linked CarPlay/accessory libraries;
- USB descriptors and runtime logs;
- existing screen, TCP, USB, and network queue profiling hooks.

Some CarPlay implementation files are supplied only as static libraries. Where
source is unavailable, this report distinguishes direct evidence from protocol
behavior inferred from symbols and runtime operation.

## 2. Executive summary

The product is not sending raw video or audio through USB CDC-ACM. It acts as a
gateway between two independent AirPlay/CarPlay sessions:

```text
iPhone
  |
  | Wi-Fi AP (ap2)
  | AirPlay/CarPlay session A
  v
+----------------------------------+
| CarPlay box                      |
|                                  |
|  receive TCP/UDP                 |
|  decrypt and authenticate A      |
|  remove AirPlay framing          |
|  process/queue H.264 or audio    |
|  construct AirPlay framing       |
|  encrypt using session B         |
+----------------------------------+
  |
  | lwIP -> en3 -> CDC-NCM -> USB bulk
  | AirPlay/CarPlay session B
  v
Head unit / USB NCM gadget
```

The box therefore terminates both security sessions. Incoming ciphertext is
not forwarded unchanged. The media payload is decrypted using session A,
parsed and queued, then repacketized and encrypted using the independent keys,
nonces/counters, and sequence state of session B.

Video is transported mainly over TCP. Its delivery, retransmission, ordering,
and backpressure are supplied by TCP, independently on each side of the box.
Audio is transported mainly as encrypted RTP over UDP. It uses RTP sequence
numbers, timestamps, jitter buffering, RTCP/timing, and feedback rather than a
TCP-style ACK for every packet.

## 3. USB transport: CDC-NCM, not CDC-ACM

The current build enables:

```make
CONFIG_USBH_CDC_NCM=1
```

The CDC-ACM starter is present but commented out in `src/main.c`:

```c
// usb_ref_host_acm_start();
```

Runtime USB enumeration also reports separate endpoint groups:

- CDC-NCM bulk IN/OUT endpoints for Ethernet traffic;
- iAP2 bulk IN/OUT endpoints for iAP2 messages;
- an interrupt endpoint used by CDC-NCM notification/status handling.

The effective media path is therefore:

```text
AirPlay TCP/UDP socket
 -> lwIP
 -> Ethernet frame on en3
 -> CDC-NCM NTB framing
 -> USB bulk transaction
```

The log label `USB_AAC_CMD_START_NCM` is an internal accessory module name. It
does not mean CDC-ACM and should not be interpreted as the AAC audio codec.

The iAP2 bulk endpoints are still important for accessory control, session
messages, and mode management, but they are not the path carrying the large
H.264 stream.

## 4. Sessions and negotiated channels

After authentication, the box has two roles:

1. AirPlay/CarPlay receiver toward the iPhone on the Wi-Fi AP interface.
2. AirPlay/CarPlay client/sender toward the head unit on the USB NCM interface.

SETUP and information exchange negotiate dynamic ports and stream parameters.
Runtime logs contain `eventPort`, `timingPort`, and `dataPort`, followed by
threads such as `AirPlayEvent_TCPClientThread`, `AirPlayTimer_Thread`, and
`AirPlayScreen_ScreenThread`.

The resulting logical channels are:

| Channel | Typical transport | Purpose |
| --- | --- | --- |
| Control | TCP | SETUP, TEARDOWN, mode and resource control |
| Event | Encrypted TCP transport | CarPlay events, commands, HID/control |
| Screen/video | TCP | Framed H.264 screen stream |
| Main/alternate audio | RTP/UDP | Media, speech, telephony and alert audio |
| Timing | UDP/RTCP/NTP-like exchange | Clock and presentation synchronization |
| Feedback | TCP/HTTP and RTCP-related exchange | Stream status, latency and feedback |

## 5. Video path

### 5.1 iPhone to box

The iPhone sends an already compressed H.264 screen bitstream. It does not send
raw RGB or YUV frames in the normal forwarding path.

The receiver path is approximately:

```text
Wi-Fi RX
 -> lwIP TCP receive
 -> AirPlayReceiverSessionScreen
 -> read AirPlay screen frame header/body
 -> decrypt and authenticate
 -> interpret frame type and timestamp
 -> process AVCC/H.264 NAL units
 -> application/bridge callback
```

Direct library evidence includes the following symbols:

- `AirPlayReceiverSessionScreen_ProcessFrames`
- `AirPlayReceiverSessionScreen_SetChaChaSecurityInfo`
- `chacha20_poly1305_decrypt`
- `chacha20_poly1305_verify`
- `AES_CTR_Update`
- `ScreenStreamProcessData`
- `H264GetNextNALUnit`
- `H264ConvertAVCCtoAnnexBHeader`

This proves that the receiver performs protocol framing, decryption, integrity
checking, timestamps/frame handling, and H.264 container/NAL processing. It is
not a ciphertext pass-through.

The library also exposes decoder/render interfaces. Those allow other product
modes to decode or display the stream, but the bridge path can continue using
the compressed H.264 bitstream without decoding to pixels and re-encoding.

### 5.2 Box to head unit

The forwarding callback chain visible in the linked libraries is:

```text
CarPlay receiver callback
 -> lib_link_cb_send_video()
 -> CAccessory_SendVideo()
 -> lib_accessory_send_video()
 -> mod_accessory_send_video()
 -> acc_carplay_send_video()
 -> AirPlayScreen_SendVideo()
```

The outbound screen sender then performs operations such as:

1. Queueing the compressed frame in `screenFrames`.
2. Extracting SPS/PPS/IDR information for initial configuration.
3. Constructing a screen configuration frame when required.
4. Constructing normal screen frame headers.
5. Adding timing/NTP information.
6. Encrypting with the security context of session B.
7. Sending the framed data using `lwip_write()` on the screen TCP socket.
8. Passing the resulting IP traffic through `en3` and CDC-NCM.

Relevant sender symbols include:

- `AirPlayScreen_SendVideo`
- `AirPlayScreen_SendScreenConfigFrame`
- `AirPlayScreen_SendScreenNormalFrame`
- `AirPlayScreen_SendKeepAliveWithBody`
- `GetH264SPSPPSIDRData`
- `AirPlayScreen_EncryptData`
- `lwip_write`

The existing `screen_queue_profiler.c` confirms the practical boundary: it
wraps `AirPlayScreen_SendVideo`, the internal vector queue operations,
`lwip_recv`, and `lwip_write`.

### 5.3 Video ACK and backpressure

No evidence indicates an application ACK for every H.264 frame. Reliability is
primarily provided by TCP:

- ACK and retransmission;
- in-order delivery;
- receive and send windows;
- congestion control;
- socket backpressure.

The two TCP connections are independent:

```text
iPhone -- TCP session A --> box -- TCP session B --> head unit
```

An ACK sent to the iPhone means that session A accepted data into its TCP receive
state. It does not mean that the same video frame has already reached the head
unit. There is no end-to-end relay of the original TCP ACK.

If the head-unit side becomes slow, the session B send window shrinks and
`lwip_write()` may block or return slowly. The internal `screenFrames` queue can
temporarily absorb the rate difference. Sustained congestion can then propagate
back through task scheduling and buffers, but it is not a single shared TCP
window across the gateway.

Previous screen queue profiling showed a normal current depth near zero and a
small observed peak, indicating no sustained video accumulation in the measured
workload.

## 6. Audio path

### 6.1 iPhone to box

Audio primarily uses RTP over UDP. The receive path is approximately:

```text
Wi-Fi UDP packet
 -> parse RTP header
 -> use RTP header as ChaCha AAD
 -> decrypt payload
 -> verify Poly1305 tag
 -> check sequence number/timestamp
 -> RTP jitter buffer
 -> optional decode/format/rate conversion
 -> audio callback or ring buffer
```

Direct library evidence includes:

- `AirPlayAudio_GetAADFromRTPHeader`
- `AirPlayAudio_DecryptPacket`
- `_GeneralAudioReceiveRTP`
- `_GeneralAudioReceiveRTCP`
- `RTPJitterBufferInit/Read/...`
- `AudioConverterFillComplexBuffer`
- `_AudioDecoderDecodeFrame`
- `RingBuffer_push/pop`
- `lwip_recvfrom`

The exact audio processing depends on the negotiated stream. Main audio,
alternate audio, media, speech, telephony and alert streams may have different
formats. Consequently, audio is not always a simple decrypt-and-copy operation;
decode, encode, sample conversion or rate conversion may be needed.

### 6.2 Box to head unit

The outbound path is approximately:

```text
audio callback/ring buffer
 -> AirPlayAudio_SendAudio()
 -> create RTP header, sequence and timestamp
 -> encrypt with session B
 -> append authentication tag
 -> lwip_sendto()
 -> UDP/IP -> CDC-NCM -> USB
```

Relevant symbols include:

- `AirPlayAudio_SetAudioStreamPacket`
- `AirPlayAudio_EncryptAudio`
- `AirPlayAudio_SendAudio`
- `chacha20_poly1305_encrypt/final`
- `lwip_sendto`

Microphone audio travels in the opposite logical direction. The head unit's
microphone samples are read through `AirPlayAudio_ReadMic()`, encoded or
formatted as negotiated, encrypted for the iPhone-side session, and sent toward
the iPhone.

### 6.3 Audio ACK, loss, and feedback

UDP/RTP has no TCP-style ACK for every packet. Audio delivery instead uses:

- RTP sequence numbers to detect loss and reordering;
- RTP timestamps for presentation timing;
- jitter buffers to absorb arrival variation;
- RTCP/timing communication;
- AirPlay feedback and latency/status exchange;
- audio ring buffers between producers and consumers.

The libraries contain `_GeneralAudioReceiveRTCP`, `AirPlayAudio_FeedBack`,
`AirPlayClient_SendMessageFeedback`, `/feedback`, and
`AirPlayTimer_SetTimeRTCPData`.

These symbols confirm the presence of feedback and RTCP-related behavior, but
they do not prove that every missing RTP packet is retransmitted. Real-time audio
commonly prefers dropping data that is already too late instead of blocking the
stream for TCP-like retransmission. Detailed resend behavior may also depend on
the negotiated stream type.

## 7. Encryption boundary

The logical data lifetime is:

```text
session A ciphertext
 -> decrypt and authenticate
 -> plaintext media payload
 -> protocol processing and queueing
 -> encrypt using session B
 -> session B ciphertext
```

The two security contexts have independent:

- session keys;
- nonce/counter state;
- frame or packet sequence state;
- authentication state.

The libraries contain ChaCha20-Poly1305, AES-CTR screen support, and some
AES-CBC-frame audio support. The selected algorithm depends on the negotiated
protocol version and stream type. Current runtime ChaCha statistics show that
most media bytes in the tested session pass through the hardware ChaCha path,
but this does not imply that every channel and every protocol version always
uses ChaCha.

## 8. ACK and flow-control layers

| Layer | Reliability or feedback behavior |
| --- | --- |
| USB bulk | USB hardware ACK/NAK and transaction retry |
| CDC-NCM | Ethernet framing; no media-level ACK of its own |
| Video/control/event TCP | TCP ACK, retransmission, ordering, window and backpressure |
| Audio RTP/UDP | No per-packet transport ACK; sequence, timestamp, jitter and feedback |
| iAP2 | Link/session sequencing and control-message acknowledgement |
| Screen frame format | Configuration, normal and keep-alive frames; no observed per-H.264-frame ACK |

USB ACK/NAK only confirms a USB transaction between the host controller and USB
device. It does not mean that the head-unit AirPlay application has consumed a
video or audio frame. Likewise, a TCP ACK on the iPhone-facing connection does
not confirm delivery through the second connection.

## 9. Practical task mapping

The major runtime tasks align with the architecture as follows:

| Task | Main role |
| --- | --- |
| `rtw_recv_tasklet` | Receive Wi-Fi frames and hand them toward lwIP |
| `TCP_IP` | Process lwIP TCP/IP input, ACK, window and socket data |
| `AirPlayScreenReceiver` | Receive, decrypt, authenticate and parse inbound screen frames |
| `ScreenThread` | Frame, encrypt and write outbound screen data |
| `usbh_main_task` / `usbh_isr_task` | Move CDC-NCM USB transfers and service host controller events |
| Audio threads | RTP receive/send, jitter buffer, conversion and encryption |

This explains why the highest CPU consumers observed during active video are
distributed across Wi-Fi RX, TCP/IP, the screen receiver, the screen sender,
and USB host processing rather than concentrated in a single forwarding task.

## 10. Conclusions

1. The project is a dual-session CarPlay/AirPlay gateway, not a transparent
   encrypted bridge.
2. The media USB path is CDC-NCM Ethernet/IP, not CDC-ACM.
3. Video remains compressed as H.264 through the main bridge path, but is
   decrypted, parsed, reframed, queued and re-encrypted.
4. Video reliability comes mainly from two independent TCP sessions. There is
   no end-to-end ACK that crosses the box.
5. Audio is mainly encrypted RTP/UDP with sequence, timing, jitter buffering,
   RTCP and feedback, not a TCP ACK for each packet.
6. iAP2 and USB transaction ACKs exist at their respective layers, but they are
   not equivalent to confirming media consumption by the head unit.

