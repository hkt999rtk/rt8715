# wcam TX sources

The pair needs two flashable TX images. The existing color bar image keeps its reversed bars and `TX FRAME` counter. A new GC2053 image captures 1280x720 NV12 at 30 fps and sends the image without any overlay. Both share the current H.264 encoder, RTP/RTCP, 5 GHz connection and RX firmware.

Approaches considered: (1) reuse the SDK MMF ISP→H.264 pipeline and add a second output bridge; (2) feed ISP NV12 buffers to the existing TX encoder. Choose (2) because it preserves one tested H.264/RTP path and only changes the raw frame producer. The SDK's GC2053 ISP firmware must be selected for the sensor image; all other roles retain their current ISP image.

The sensor producer owns aligned DMA buffers, exchanges a free buffer in the ISP frame callback, and gives a completed frame to the encoder task. When the encoder holds a frame, the ISP may skip a new frame rather than overwrite it. TX keeps sending until Wi-Fi disconnects, then resumes with an IDR. Sensor initialization, chip ID and frame timeouts are logged. The firmware packer must both select `isp_gc2053.bin` and include the `ISP` image in the firmware image list; the SDK's default list omits it. Sensor and radio behavior require a two-board hardware test; the host build checks linked roles and image selection.
