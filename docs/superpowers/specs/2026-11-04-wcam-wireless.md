# Wireless wcam TX/RX

Approved scope: port the small ireader H.264 RTP core and generate separate
flash_wcam_tx.bin / flash_wcam_rx.bin. RX is Wi-Fi AP; TX automatically connects
and retries indefinitely. Keep the tested 1280x720/30 synthetic counter source
on TX and H.264 UVC sink on RX. No customer archives.

Use WPA2 AP WCAM-RX, POC passphrase wcam720p30, channel 6, static addresses
192.168.4.1 (RX) / 192.168.4.2 (TX), RTP UDP 5004 and RTCP UDP 5005.
PT 96, 90kHz clock, maximum RTP datagram 1200 bytes. Single NAL/FU-A send,
Single NAL/STAP-A/FU-A receive. Bound all buffers; no per-packet heap allocation.
RX assembles Annex B access units, waits for SPS/PPS/IDR after loss or UVC start.
RTCP compound SR/RR + SDES and PLI provide reports and recovery; no retransmission
in this first version. PLI retries and TX IDR requests are rate limited.
Keep existing standalone color bar build available. Network and USB buffer
ownership must not race. Do not claim hardware testing without attached boards.
