# Airbox CDC-NCM Packet Aggregation Investigation

## 1. Purpose

This document records how many Ethernet datagrams the current Airbox CDC-NCM
implementation aggregates into one Network Transfer Block (NTB).

It is important to distinguish between:

1. the sizes and limits negotiated with the NCM Gadget;
2. the limits retained by the host NCM context;
3. the number of datagrams the active Airbox transmit path actually puts in an
   NTB.

The negotiated NTB capacity alone does not prove that packet aggregation is
actually being performed.

## 2. Observed NCM negotiation

The device log reports the following parameters:

```text
dwNtbInMaxSize=2048
dwNtbOutMaxSize=16384
wNdpOutPayloadRemainder=0
wNdpOutDivisor=4
wNdpOutAlignment=4
wNtbOutMaxDatagrams=0
flags=0x11
ctx->max_ndp_size=20
```

The directions are relative to the USB host:

| Direction | Negotiated NTB size |
| --- | ---: |
| NCM Gadget to Airbox, USB IN | 2048 bytes |
| Airbox to NCM Gadget, USB OUT | 16384 bytes |

`wNtbOutMaxDatagrams=0` is not interpreted as zero usable datagrams. The host
setup code normalizes an unsupported or unrestricted value to an internal
maximum of two TX datagrams. For NDP16 this produces:

```text
max_ndp_size = (tx_max_datagrams + 3) * 4
             = (2 + 3) * 4
             = 20 bytes
```

Therefore, the initialized NCM context is capable of describing up to two TX
datagrams in its generic aggregation path.

## 3. Actual Airbox TX behavior

The current lwIP-to-NCM path is implemented in:

```text
component/common/network/lwip/lwip_v2.1.2/port/realtek/freertos/ethernetif.c
```

The active call sequence is:

```text
lwIP produces one Ethernet pbuf
    |
    v
low_level_output_mii()
    |
    v
usbh_cdc_ncm_send_data(payload, length)
    |
    v
ncm_wrap_ntb(payload, length)
    |
    v
usbh_cdc_ncm_bulk_send(NTB, NTB_length)
```

If the pbuf is chained, `low_level_output_mii()` first flattens the pbuf chain
into `TX_BUFFER`. This only reconstructs one Ethernet frame; it does not combine
multiple Ethernet packets.

Disassembly of `lib_usbsmart.a` confirms that
`usbh_cdc_ncm_send_data()` calls `ncm_wrap_ntb()` exactly once for the supplied
Ethernet frame and then immediately submits that NTB through the USB bulk OUT
endpoint. There is no pending NTB, packet queue, aggregation timeout, or second
datagram append operation in this active path.

Consequently, current Airbox TX behavior is:

```text
1 Ethernet packet -> 1 NCM NTB -> 1 USB bulk submission
```

The negotiated 16384-byte OUT NTB capacity is therefore mostly unused. The
internal two-datagram limit and `max_ndp_size=20` do not change the active
behavior because the current path bypasses the generic multi-datagram fill
mechanism.

## 4. Actual Airbox RX capability

The receive parser in `lib_usbsmart.a` supports multiple NDP16 datagram pointer
entries. It iterates through the entries in the received NDP and delivers each
valid Ethernet datagram separately to the upper callback.

The number of received datagrams per NTB is determined by the Arkmicro NCM
Gadget, not by the Airbox TX wrapper.

The practical RX constraint is `dwNtbInMaxSize=2048`:

- one near-MTU Ethernet frame of about 1500 bytes fits in an NTB;
- two near-MTU frames cannot fit in a 2048-byte NTB;
- several small ACK or control packets can theoretically share an NTB;
- the present logs do not record how many NDP entries each received NTB
  contains, so the Gadget's real small-packet aggregation ratio is not yet
  measured.

For the large media-related network frames that dominate bandwidth, RX is thus
expected to be approximately one Ethernet packet per NTB.

## 5. Current conclusion

| Direction | NTB size limit | Parser/context capability | Current effective aggregation |
| --- | ---: | --- | --- |
| Airbox to Gadget | 16384 bytes | Host context normalized to at most 2 TX datagrams | Exactly 1 packet per NTB |
| Gadget to Airbox | 2048 bytes | Multiple NDP16 entries accepted | Gadget-dependent; near-MTU traffic is normally 1 packet per NTB |

The current Airbox CDC-NCM transmit implementation does not perform useful
packet aggregation.

## 6. Recommended runtime measurement

Before changing the transport behavior, add low-overhead counters for a
10-second window:

```text
NCM TX: Ethernet packets, NTBs, bytes, datagrams-per-NTB histogram
NCM RX: NTBs, NDP datagrams, bytes, datagrams-per-NTB histogram
USB:    bulk submissions and average/max submitted length
```

Suggested aggregation histogram buckets are `1`, `2`, `3-4`, `5-8`, and
`>8` datagrams per NTB. Counters should be updated per NTB but printed only once
per window.

This will verify the Gadget's RX aggregation behavior and provide a baseline
for evaluating any TX aggregation change.

## 7. Possible TX aggregation optimization

A future Airbox TX aggregator could retain an open 16-KB NTB and flush it when
one of the following conditions occurs:

1. the configured datagram limit is reached;
2. the next packet would exceed the NTB size;
3. a short flush deadline expires;
4. a latency-sensitive packet requests immediate transmission;
5. link shutdown or an error requires the pending NTB to be completed.

The present context limit is two datagrams, so an initial implementation should
first aggregate at most two packets unless the setup and wrapper code are also
changed and validated against the NCM Gadget.

Expected benefits are fewer USB bulk submissions, fewer host-controller service
operations, and fewer completion events. The tradeoff is added packet latency
while waiting for another packet, so the flush deadline must remain short and
must be measured with CarPlay traffic.

## 8. Evidence used

- Runtime NCM negotiation log shown above.
- `low_level_output_mii()` in
  `component/common/network/lwip/lwip_v2.1.2/port/realtek/freertos/ethernetif.c`.
- Disassembly of `ncm.o`, `cdc_ncm.o`, and `usbh_cdc_ncm_hal.o` from
  `project/realtek_amebapro_v0_example/GCC-RELEASE/usb_lib/lib_usbsmart.a`.

