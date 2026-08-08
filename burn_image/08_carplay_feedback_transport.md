# CarPlay 媒體承載與 Feedback 調查

## 1. 調查結論

目前 CarPlay/AirPlay 整合不是以同一條 RTSP/RTP 路徑承載所有媒體：

- RTSP-like TCP connection 負責 `SETUP`、`RECORD`、`FLUSH`、`TEARDOWN`
  等 session control。
- H.264 screen payload 使用 SETUP 協商出的獨立 TCP `dataPort`，採用
  AirPlay Screen 自訂 framing，不是 RTP。
- Audio payload 主要使用 RTP/UDP，並使用 AirPlay 自訂的 RTCP-like
  retransmission protocol。
- Feedback 不只有 RTCP；還包括 TCP flow control、`POST /feedback`、
  timing exchange、`forceKeyFrame` 與 event/control commands。

因此，不應把 `/feedback`、audio RTCP retransmission 與 video TCP ACK
視為同一種 feedback。它們位於不同的協定層，解決的問題也不同。

## 2. 調查依據與限制

主要依據目前連結的二進位 library：

- `GCC-RELEASE/carplay_app/lib_CarPlay.a`
- `GCC-RELEASE/carplay_app/lib_Accessory2.a`

透過 symbol、string、relocation 與 disassembly 確認呼叫關係和 packet
常數。部分 CarPlay protocol source code 不在 repository，因此欄位的語意
以 library 行為為準；無法由二進位直接證明的細節不做過度推論。

## 3. Video payload 並非 RTP

RTSP-like control connection 可看到以下 methods：

```text
ANNOUNCE
SETUP
RECORD
PAUSE
FLUSH
TEARDOWN
OPTIONS
POST
GET
PUT
```

這條 connection 用於 session negotiation 和 control。SETUP 回應會提供
`dataPort`、`eventPort` 與 `timingPort`，實際 H.264 screen stream 則建立在
獨立 TCP `dataPort` 上。

Video receive/send path 可概括為：

```text
iPhone
  -> TCP dataPort
  -> AirPlay screen frame header/body
  -> decrypt and authenticate
  -> frame type/timestamp handling
  -> AVCC/H.264 NAL processing
  -> application bridge callback
  -> construct new AirPlay screen frame
  -> encrypt for the other session
  -> TCP dataPort
  -> head unit
```

相關 symbols 包括：

- `AirPlayReceiverSessionScreen_ProcessFrames`
- `ScreenStreamProcessData`
- `H264GetNextNALUnit`
- `AirPlayScreen_SendVideo`
- `AirPlayScreen_SendScreenConfigFrame`
- `AirPlayScreen_SendScreenNormalFrame`
- `AirPlayScreen_SendKeepAliveWithBody`
- `AirPlayScreen_EncryptData`
- `lwip_recv`
- `lwip_write`

這條路徑沒有使用 RTP sequence number，也沒有看到針對每個 H.264 frame
設計的 application ACK。可靠性主要由 TCP 提供：

- ACK 與 retransmission；
- in-order delivery；
- congestion control；
- receive/send window；
- socket backpressure。

Box 會終止兩邊獨立的 TCP session。iPhone 收到的 TCP ACK 只表示資料已被
Box 這一側的 TCP stack 接收，不表示相同 video frame 已抵達 head unit。

## 4. `POST /feedback`

Outbound CarPlay client 有一條固定 feedback thread：

```text
AirPlayClient_FeedbackThread
  -> vTaskDelay(2000)
  -> AirPlayClient_SendMessageFeedback
  -> POST /feedback
```

目前 `configTICK_RATE_HZ=1000`，所以 `vTaskDelay(2000)` 約等於每 2 秒執行
一次。

`POST /feedback` 本身沒有送出大量 media body。對端回覆 binary plist，
解析流程為：

```text
AirPlayResponse_GetInfoFeedbackResponse
  -> read "streams" array
  -> for each stream dictionary:
       read "type"
       read "sr"
  -> AirPlayAudio_FeedBack
  -> acc_carplay_cb_feed_back
```

已由 `.rodata` 與 disassembly 確認 response 欄位：

```text
streams[]
  type : integer
  sr   : floating-point value
```

`type` 用於識別 audio stream，例如 100 或 101。`sr` 會進入
`AirPlayAudio_FeedBack()` 的 averaging/rate calculation，主要用途是 audio
sample-rate、clock drift 或 rate/skew correction，而不是 video frame ACK。

## 5. Audio RTP 與 RTCP-like retransmission

Audio payload 使用 RTP/UDP。Receive path 包含：

```text
UDP receive
  -> parse RTP header
  -> decrypt/authenticate payload
  -> sequence/timestamp check
  -> jitter buffer
  -> audio processing/output
```

相關 symbols：

- `_GeneralAudioReceiveRTP`
- `_GeneralAudioReceiveRTCP`
- `AirPlayAudio_GetAADFromRTPHeader`
- `AirPlayAudio_DecryptPacket`
- `RTPJitterBufferInit`
- `RTPJitterBufferRead`

### 5.1 Missing packet request: type `0xD5`

`_GeneralAudioReceiveRTP()` 偵測到 RTP sequence gap 後會建立 8-byte
RTCP-like packet。Disassembly 明確寫入 packet type `0xD5`，再用
`lwip_sendto()` 送往 audio control/RTCP port。

這個 request 包含缺少的起始 sequence number 與要求重傳的 packet 數量。
它是 AirPlay/AirTunes 自訂的 selective retransmission request，不是 TCP
ACK，也不是一般 RFC RTCP Receiver Report。

### 5.2 Retransmission response: type `0xD6`

`_GeneralAudioReceiveRTCP()` 明確接受 packet type `0xD6`：

- 若 packet 長度為 8 bytes，會呼叫 `_RetransmitsAbortOne()`，代表該次要求
  已結束但沒有可用的 RTP payload，例如 `FUTILE`/not found。
- 若 packet 長度大於 15 bytes，會移除 RTCP-like wrapper，將其中的 RTP
  packet 重新送入 `_GeneralAudioReceiveRTP()`。

因此 audio 並非每包都有 ACK，而是在偵測到 loss 後才選擇性要求重傳。
已經太晚或無法取得的 packet 可以放棄，不會像 TCP 一樣阻塞整條 stream。

### 5.3 Audio loss/retransmit 統計

Library 中可確認以下統計與狀態：

- retransmit requests sent；
- retransmit responses received；
- futile responses；
- sequence not found；
- retransmit response latency；
- retry latency/count；
- lost packets；
- unrecovered packets；
- late packets；
- maximum loss burst；
- big losses；
- audio glitches。

這些統計主要服務 audio quality/loss recovery，不代表 video 使用 RTCP。

## 6. Timing feedback

系統另外協商 `timingPort`，並執行 NTP-like request/response：

```text
_TimingSendRequest
_TimingReceiveResponse
_TimingThread
```

這條路徑用來估算：

- round-trip time；
- sender/receiver clock offset；
- clock steps；
- presentation timestamp alignment。

Timing exchange 使用 UDP 並帶有 RTCP/NTP-like 特徵，但它不是 H.264 或
audio payload，也不是逐 packet delivery ACK。

## 7. Video recovery 與 control feedback

Event/control dictionary 中可確認：

```text
forceKeyFrame
updateFeedback
```

`forceKeyFrame` 可在 H.264 decoder/receiver 失去同步時要求來源重新產生
IDR/key frame。這是 video recovery command，不是每 frame ACK。

`updateFeedback` 是 feedback state/command 的更新入口，與 `/feedback`
refresh 機制相關；它也不承載 video payload。

此外還有 keepalive、mode/resource updates、HID/event messages。這些屬於
session liveness 或 CarPlay control plane，不應和 media retransmission 混為
一談。

## 8. iAP2 retransmission 不等於媒體 retransmission

Library 也包含：

- `iAP2LinkActionResendData`
- `iAP2LinkActionResendMissing`
- `iAP2PacketCheckDetectNACK`

這些屬於 iAP2 link/session transport 的可靠性機制。它們保護 iAP2 control
messages，不代表 H.264 dataPort 使用 RTP，也不是 audio 的 D5/D6
retransmission protocol。

## 9. Feedback 類型總表

| 對象 | 承載方式 | Feedback/recovery |
| --- | --- | --- |
| H.264 screen | 自訂 AirPlay framing over TCP `dataPort` | TCP ACK/window/retransmission、`forceKeyFrame` |
| Audio media | RTP/UDP | sequence/timestamp、jitter buffer、D5/D6 selective retransmission |
| Audio rate | `POST /feedback`，約每 2 秒 | `streams[].type`、`streams[].sr` rate/skew correction |
| Clock sync | UDP `timingPort` | RTT、clock offset、clock step |
| Session control | RTSP-like TCP/event channel | SETUP/RECORD/FLUSH/TEARDOWN、mode/resource/control updates |
| iAP2 control | iAP2 bulk/session transport | iAP2 ACK/NACK/resend，與 media feedback 分離 |

## 10. 最終判斷

1. H.264 payload 不是 RTP；RTSP-like connection 只負責 control，video 使用
   獨立 TCP `dataPort` 與 AirPlay Screen framing。
2. Audio payload 才主要使用 RTP/UDP。
3. Audio loss recovery 使用 AirPlay 自訂的 `0xD5` request / `0xD6`
   response protocol。
4. `/feedback` 約每 2 秒交換 audio stream 的 `type` 與 `sr`，主要用於
   sample-rate/clock drift correction。
5. Video reliability 主要依靠 TCP；需要內容層恢復時可使用
   `forceKeyFrame`。
6. Feedback 不是單一 RTCP channel，而是分散在 TCP transport、HTTP/RTSP-like
   feedback、audio RTCP-like retransmit、timing 與 event/control channel。
