# AAC decoder benchmark sample

`bear-audio-lc-aac.aac` is Chromium's AAC-LC test stream:

- Source: https://chromium.googlesource.com/chromium/src/media/+/refs/heads/main/test/data/bear-audio-lc-aac.aac
- SHA-256: `4d820c54c41d850fd72d4b81780fc0e007a938643181e26dacbb5da0f6d569c7`
- Format: ADTS AAC-LC, 48 kHz, stereo, approximately 144 kbit/s
- Size: 48,780 bytes; 131 access units; approximately 2.717 seconds

The firmware build copies this file into the read-only FAT image as
`fat:/bear-audio-lc-aac.aac` when `AAC_DECODER_BENCHMARK=1`.
