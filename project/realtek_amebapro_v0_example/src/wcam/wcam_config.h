#ifndef WCAM_CONFIG_H
#define WCAM_CONFIG_H
#ifndef WCAM_ROLE_TX
#define WCAM_ROLE_TX 0
#endif
#ifndef WCAM_ROLE_RX
#define WCAM_ROLE_RX 0
#endif
#ifndef WCAM_TX_SENSOR
#define WCAM_TX_SENSOR 0
#endif
#if WCAM_ROLE_TX && WCAM_ROLE_RX
#error Select one wcam role
#endif
#define WCAM_WIFI_SSID "WCAM-RX"
#define WCAM_WIFI_PASSWORD "wcam720p30"
#define WCAM_WIFI_CHANNEL 36
#define WCAM_RX_IP "192.168.4.1"
#define WCAM_TX_IP "192.168.4.2"
#define WCAM_RTP_PORT 5004
#define WCAM_RTCP_PORT 5005
#define WCAM_PLI_MS 500u
#if WCAM_ROLE_TX
#if WCAM_TX_SENSOR
#define WCAM_ROLE_NAME "TX-GC2053"
#else
#define WCAM_ROLE_NAME "TX"
#endif
#elif WCAM_ROLE_RX
#define WCAM_ROLE_NAME "RX"
#else
#define WCAM_ROLE_NAME "LOCAL"
#endif
#endif
