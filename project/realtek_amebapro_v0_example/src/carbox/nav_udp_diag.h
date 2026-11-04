/* Navigation UDP evidence: explicit local-port registration, bounded storage. */
#ifndef CARBOX_NAV_UDP_DIAG_H
#define CARBOX_NAV_UDP_DIAG_H
#include <stdint.h>
#ifndef CONFIG_AUDIO_NAV_DIAG
#define CONFIG_AUDIO_NAV_DIAG 0
#endif
#if CONFIG_AUDIO_NAV_DIAG
struct pbuf;
enum carbox_nav_udp_result {
    NAV_UDP_ACCEPTED, NAV_UDP_NO_CONN, NAV_UDP_INVALID_MBOX,
    NAV_UDP_RCVBUF_FULL, NAV_UDP_NETBUF_NOMEM, NAV_UDP_MBOX_FULL,
    NAV_UDP_RESULT_COUNT
};
void carbox_nav_udp_register(uint16_t port, uint32_t session_id);
void carbox_nav_udp_unregister(uint16_t port);
uint32_t carbox_nav_udp_begin(uint16_t port, const struct pbuf *p);
void carbox_nav_udp_finish(uint32_t token, enum carbox_nav_udp_result result);
/* Call only from the existing low-priority statistics task. */
void carbox_nav_udp_report(void);
#endif
#endif
