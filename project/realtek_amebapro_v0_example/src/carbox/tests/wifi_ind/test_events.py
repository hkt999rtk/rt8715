#!/usr/bin/env python3
"""Exercise the real Wi-Fi dispatcher and STA handlers with host lwIP/RTOS stubs.

The test catches missing link-down, WPA association mistaken for join success,
AP-client departure mistaken for STA disconnect, and full callback tables
preventing internal join processing. No radio or scheduling is emulated.
"""
from pathlib import Path
import re
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[6]
WIFI = REPO / "component/common/api/wifi"


def function(source, name):
    match = re.search(r"^(?:static )?(?:void|int) " + name + r"\s*\(", source, re.M)
    if not match:
        return ""
    start = source.index("{", match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end] + "\n"


MOCKS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#define CONFIG_WIFI_IND_USE_THREAD 0
#define CONFIG_LWIP_LAYER 1
#define LWIP_VERSION_MAJOR 2
#define LWIP_VERSION_MINOR 1
#define LWIP_IPV6 1
#define CONFIG_ENABLE_P2P 0
#define CONFIG_INIC_EN 0
#define NET_IF_NUM 3
#define ETH_ALEN 6
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef unsigned long rtw_result_t;
typedef unsigned long rtw_event_indicate_t;
typedef int rtw_mode_t;
typedef int rtw_security_t;
typedef void *_sema;
enum { RTW_SUCCESS=0, RTW_BADARG=-2, RTW_ERROR=-1, RTW_TIMEOUT=2,
       RTW_INVALID_KEY=4, RTW_BUSY=-16, RTW_NORESOURCE=-22, RTW_NOMEM=-27 };
#define RTW_FALSE 0
#define RTW_TRUE 1
#define RTW_MAX_PSK_LEN 64
#define RTW_MIN_PSK_LEN 8
#define RTW_JOIN_TIMEOUT 15000
enum { RTW_MODE_NONE=0, RTW_MODE_STA=1, RTW_MODE_AP=2, RTW_MODE_STA_AP=3 };
enum { RTW_UNKNOWN=-1, RTW_NO_ERROR=0, RTW_NONE_NETWORK=1,
       RTW_CONNECT_FAIL=2, RTW_4WAY_HANDSHAKE_TIMEOUT=3, RTW_WRONG_PASSWORD=4 };
enum { RTW_SECURITY_OPEN=0, RTW_SECURITY_WEP_PSK=1, RTW_SECURITY_WEP_SHARED=2,
       RTW_SECURITY_WPA2_AES_PSK=3, RTW_SECURITY_WPA2_TKIP_PSK=4,
       RTW_SECURITY_WPA2_MIXED_PSK=5, RTW_SECURITY_WPA_WPA2_AES_PSK=6,
       RTW_SECURITY_WPA_WPA2_TKIP_PSK=7, RTW_SECURITY_WPA_WPA2_MIXED_PSK=8,
       RTW_SECURITY_WPA_AES_PSK=9, RTW_SECURITY_WPA_TKIP_PSK=10,
       RTW_SECURITY_WPA_MIXED_PSK=11 };
typedef void (*rtw_event_handler_t)(char *, int, int, void *);
typedef struct { rtw_event_handler_t handler; void *handler_user_data; } event_list_elem_t;
struct netif { int link, dhcp4, dhcp6; } xnetif[NET_IF_NUM];
typedef struct {
    struct { int len; unsigned char val[33]; } ssid;
    struct { unsigned char octet[6]; } bssid;
    int security_type;
    unsigned char *password;
    int password_len, key_id;
} rtw_network_info_t;
struct join { rtw_network_info_t network_info; void *join_sema; };
typedef struct join internal_join_result_t;
static struct join join, *join_user_data;
static unsigned char ap_bssid[6];
static uint32_t rtw_join_status;
static int error_flag, wifi_mode, wakes, callbacks, diagnostics, expected_link;
static int link_up_calls, link_down_calls, test_sema;
#define rtw_memcpy memcpy
#define rtw_memset memset
static int netif_is_link_up(struct netif *n) { return n->link; }
static void netif_set_link_up(struct netif *n) { n->link=1; link_up_calls++; }
static void netif_set_link_down(struct netif *n) { n->link=0; link_down_calls++; }
void dhcp_stop(struct netif *n) { n->dhcp4=0; }
void dhcp6_stop(struct netif *n) { n->dhcp6=0; }
static void rtw_up_sema(void **s) {
    assert(xnetif[0].link == expected_link);
    assert(*s);
    (*(int *)*s)++; wakes++;
}
static int test_printf(const char *format, ...) { (void)format; diagnostics++; return 0; }
#define printf test_printf
#define RTW_API_INFO(...) test_printf(__VA_ARGS__)
'''

JOIN_MOCKS = r'''
void wifi_indication(rtw_event_indicate_t, char *, int, int);
static int wait_outcome, driver_connected;
static int wifi_is_connected_to_ap(void) { return driver_connected ? RTW_SUCCESS : RTW_ERROR; }
static int wifi_disconnect(void) { return 0; }
static void rtw_msleep_os(int ms) { (void)ms; }
static void rtw_mdelay_os(int ms) { (void)ms; }
static void *rtw_zmalloc(unsigned n) { return calloc(1,n); }
static void rtw_free(void *p) {
    /* A late event must never see a result or password that is being freed. */
    assert(!join_user_data || (p != join_user_data && p != join_user_data->network_info.password));
    free(p);
}
static void rtw_init_sema(void **s, int initial) { *s=malloc(sizeof(int)); *(int *)*s=initial; }
static void rtw_free_sema(void **s) {
    assert(!join_user_data || join_user_data->join_sema != *s);
    free(*s); *s=NULL;
}
static int wifi_connect_local(rtw_network_info_t *n) { (void)n; return 0; }
static int wifi_connect_bssid_local(rtw_network_info_t *n) { (void)n; return 0; }
static int rtw_down_timeout_sema(void **s, unsigned timeout) {
    char mac[6]={1,2,3,4,5,6};
    (void)timeout;
    assert(join_user_data && join_user_data->join_sema==*s);
    if (wait_outcome==0) return RTW_FALSE;
    if (wait_outcome==1) {
        wifi_indication(WIFI_EVENT_CONNECT, mac, sizeof(mac), 0);
        expected_link=1;
        wifi_indication(WIFI_EVENT_FOURWAY_HANDSHAKE_DONE, NULL, 0, 0);
    } else {
        wifi_indication(WIFI_EVENT_NO_NETWORK, NULL, 0, 0);
    }
    return RTW_TRUE;
}
'''

TESTS = r'''
static void observer(char *buf, int len, int flags, void *userdata) {
    (void)buf; (void)len; (void)flags;
    assert(userdata == &callbacks);
    assert(xnetif[0].link == expected_link);
    callbacks++;
}
static void observer2(char *b, int n, int f, void *u) { observer(b,n,f,u); }
static void observer3(char *b, int n, int f, void *u) { observer(b,n,f,u); }
static void observer4(char *b, int n, int f, void *u) { observer(b,n,f,u); }
static void reset(int mode, int security) {
    init_event_callback_list();
    memset(xnetif, 0, sizeof(xnetif));
    memset(&join, 0, sizeof(join));
    memset(ap_bssid, 0, sizeof(ap_bssid));
    test_sema=0;
    join.join_sema=&test_sema;
    join.network_info.security_type=security;
    join_user_data=&join;
    wifi_mode=mode;
    error_flag=RTW_UNKNOWN;
    rtw_join_status=JOIN_CONNECTING;
    wakes=callbacks=diagnostics=expected_link=link_up_calls=link_down_calls=0;
    xnetif[1].link=xnetif[2].link=1;
}
int main(void) {
    char mac[6]={1,2,3,4,5,6};
    char lost[8]={1,2,3,4,5,6,8,0};

    /* No public callback registration may be required for internal cleanup. */
    reset(RTW_MODE_STA_AP, RTW_SECURITY_WPA2_AES_PSK);
    join_user_data=NULL;
    xnetif[0].link=xnetif[0].dhcp4=xnetif[0].dhcp6=1;
    wifi_indication(WIFI_EVENT_DISCONNECT, lost, sizeof(lost), 0);
    assert(!xnetif[0].link && !xnetif[0].dhcp4 && !xnetif[0].dhcp6);
    assert(xnetif[1].link && xnetif[2].link && !wakes);

    /* WPA association alone must not announce a usable link or wake a waiter. */
    reset(RTW_MODE_STA_AP, RTW_SECURITY_WPA2_AES_PSK);
    wifi_reg_event_handler(WIFI_EVENT_CONNECT, observer, &callbacks);
    wifi_reg_event_handler(WIFI_EVENT_FOURWAY_HANDSHAKE_DONE, observer, &callbacks);
    wifi_indication(WIFI_EVENT_CONNECT, mac, sizeof(mac), 0);
    assert(callbacks==1 && !wakes && !xnetif[0].link);
    expected_link=1;
    wifi_indication(WIFI_EVENT_FOURWAY_HANDSHAKE_DONE, NULL, 0, 0);
    assert(callbacks==2 && wakes==1 && xnetif[0].link && link_up_calls==1);
    /* After the caller leaves, rekey is not a new blocking connection. */
    join_user_data=NULL;
    rtw_join_status &= ~JOIN_CONNECTING;
    wifi_indication(WIFI_EVENT_FOURWAY_HANDSHAKE_DONE, NULL, 0, 0);
    assert(!(rtw_join_status & JOIN_CONNECTING) && wakes==1 && link_up_calls==1);
    expected_link=0;
    wifi_indication(WIFI_EVENT_DISCONNECT, lost, sizeof(lost), 0);
    wifi_indication(WIFI_EVENT_FOURWAY_HANDSHAKE_DONE, NULL, 0, 0);
    assert(!xnetif[0].link && !(rtw_join_status & JOIN_LINK_READY));

    /* Open/WEP association completes the join without a four-way event. */
    for (int security=RTW_SECURITY_OPEN; security<=RTW_SECURITY_WEP_SHARED; security++) {
        reset(RTW_MODE_STA, security);
        expected_link=1;
        wifi_reg_event_handler(WIFI_EVENT_CONNECT, observer, &callbacks);
        wifi_indication(WIFI_EVENT_CONNECT, mac, sizeof(mac), 0);
        assert(xnetif[0].link && wakes==1 && callbacks==1);
    }

    /* Disconnect state and DHCP cleanup are visible before application callbacks. */
    reset(RTW_MODE_STA_AP, RTW_SECURITY_WPA2_AES_PSK);
    rtw_join_status |= JOIN_COMPLETE | JOIN_LINK_READY | JOIN_HANDSHAKE_DONE;
    xnetif[0].link=xnetif[0].dhcp4=xnetif[0].dhcp6=1;
    wifi_reg_event_handler(WIFI_EVENT_DISCONNECT, observer, &callbacks);
    wifi_indication(WIFI_EVENT_DISCONNECT, lost, sizeof(lost), 0);
    assert(callbacks==1 && wakes==1 && !xnetif[0].dhcp4 && !xnetif[0].dhcp6);
    assert(!(rtw_join_status & (JOIN_COMPLETE | JOIN_LINK_READY | JOIN_HANDSHAKE_DONE)));
    assert(xnetif[1].link && xnetif[2].link);

    /* Failed scans finish the pending join, rather than waiting for a timeout. */
    reset(RTW_MODE_STA, RTW_SECURITY_WPA2_AES_PSK);
    wifi_reg_event_handler(WIFI_EVENT_NO_NETWORK, observer, &callbacks);
    wifi_indication(WIFI_EVENT_NO_NETWORK, NULL, 0, 0);
    assert(wakes==1 && callbacks==1 && error_flag==RTW_NONE_NETWORK);

    /* AP-client events must never stop the STA DHCP client or lower either link. */
    reset(RTW_MODE_STA_AP, RTW_SECURITY_OPEN);
    expected_link=xnetif[0].link=xnetif[0].dhcp4=xnetif[0].dhcp6=1;
    wifi_reg_event_handler(WIFI_EVENT_STA_DISASSOC, observer, &callbacks);
    wifi_indication(WIFI_EVENT_STA_DISASSOC, mac, sizeof(mac), 0);
    assert(callbacks==1 && !wakes && xnetif[0].dhcp4 && xnetif[0].dhcp6 && xnetif[1].link);
    wifi_mode=RTW_MODE_AP;
    wifi_indication(WIFI_EVENT_DISCONNECT, lost, sizeof(lost), 0);
    assert(xnetif[0].link && xnetif[0].dhcp4 && !wakes);

    /* Public listener exhaustion is reported, and cannot suppress internal work. */
    reset(RTW_MODE_STA, RTW_SECURITY_OPEN);
    wifi_reg_event_handler(WIFI_EVENT_CONNECT, observer, &callbacks);
    wifi_reg_event_handler(WIFI_EVENT_CONNECT, observer2, &callbacks);
    wifi_reg_event_handler(WIFI_EVENT_CONNECT, observer3, &callbacks);
    wifi_reg_event_handler(WIFI_EVENT_CONNECT, observer, &callbacks);
    assert(!diagnostics); /* Existing registration is not an overflow. */
    wifi_reg_event_handler(WIFI_EVENT_CONNECT, observer4, &callbacks);
    assert(diagnostics==1);
    expected_link=1;
    wifi_indication(WIFI_EVENT_CONNECT, mac, sizeof(mac), 0);
    assert(callbacks==3 && wakes==1);
    wifi_unreg_event_handler(WIFI_EVENT_CONNECT, observer2);
    wifi_reg_event_handler(WIFI_EVENT_CONNECT, observer4, &callbacks);
    wifi_indication(WIFI_EVENT_CONNECT, mac, sizeof(mac), 0);
    assert(callbacks==6);

    /* Invalid events cannot index the callback table or mutate the STA. */
    reset(RTW_MODE_STA, RTW_SECURITY_OPEN);
    wifi_indication((rtw_event_indicate_t)-1, NULL, 0, 0);
    wifi_indication(WIFI_EVENT_MAX, NULL, 0, 0);
    wifi_indication(WIFI_EVENT_CONNECT, mac, -1, 0);
    wifi_indication(WIFI_EVENT_CONNECT, NULL, 6, 0);
    wifi_indication(WIFI_EVENT_CONNECT, mac, 2, 0);
    assert(!wakes && !callbacks && !xnetif[0].link);

    /* Truncated/missing reason cannot cause unaligned or out-of-bounds reads. */
    reset(RTW_MODE_STA, RTW_SECURITY_WPA2_AES_PSK);
    wifi_indication(WIFI_EVENT_DISCONNECT, mac, sizeof(mac), 0);
    wifi_indication(WIFI_EVENT_DISCONNECT, NULL, 0, 0);
    /* Reason 15 is decoded safely even from an odd-address event buffer. */
    reset(RTW_MODE_STA, RTW_SECURITY_WPA2_AES_PSK);
    _Alignas(4) char unaligned[9]={0,1,2,3,4,5,6,15,0};
    wifi_indication(WIFI_EVENT_CONNECT, mac, sizeof(mac), 0);
    wifi_indication(WIFI_EVENT_DISCONNECT, unaligned+1, 8, 0);
    assert(error_flag==RTW_4WAY_HANDSHAKE_TIMEOUT && wakes==1);

    /* Direct callback dispatch validates its signed event number too. */
    assert(rtw_indicate_event_handle(-1, NULL, 0, 0)==(rtw_result_t)RTW_BADARG);
    assert(rtw_indicate_event_handle(WIFI_EVENT_MAX, NULL, 0, 0)==(rtw_result_t)RTW_BADARG);
    /* Exercise both public join APIs: timeout, success and scan failure.
     * The radio is mocked; actual wait/cleanup/control flow is compiled below. */
    for (int bssid=0; bssid<2; bssid++) {
        for (int outcome=0; outcome<3; outcome++) {
            reset(RTW_MODE_STA, RTW_SECURITY_WPA2_AES_PSK);
            join_user_data=NULL;
            rtw_join_status=0;
            wait_outcome=outcome;
            driver_connected=1; /* stale driver-ready must not override NO_NETWORK */
            int ret = bssid ? wifi_connect_bssid((unsigned char *)mac, "test",
                RTW_SECURITY_WPA2_AES_PSK, "password", 6, 4, 8, -1, NULL) :
                wifi_connect("test", RTW_SECURITY_WPA2_AES_PSK, "password", 4, 8, -1, NULL);
            assert(ret==(outcome==0 ? RTW_TIMEOUT : outcome==1 ? RTW_SUCCESS : RTW_ERROR));
            assert(!join_user_data && !(rtw_join_status & JOIN_CONNECTING));
        }
    }
    puts("wifi event lifecycle: PASS");
    return 0;
}
'''


def main():
    conf = (WIFI / "wifi_conf.c").read_text()
    constants = (REPO / "component/common/drivers/wlan/realtek/include/wifi_constants.h").read_text()
    events = re.search(r"enum _WIFI_EVENT_INDICATE\s*\{.*?\};", constants, re.S).group()
    bits = "\n".join(re.findall(r"^#define JOIN_.*", conf, re.M))
    handlers = "".join(function(conf, name) for name in (
        "wifi_sta_link_up", "wifi_no_network_hdl", "wifi_connected_hdl",
        "wifi_handshake_done_hdl", "wifi_disconn_hdl", "wifi_event_sta_internal_hdl"))
    dispatcher = re.sub(r'^#include .*$', '', (WIFI / "wifi_ind.c").read_text(), flags=re.M)
    joins = function(conf, "wifi_connect") + function(conf, "wifi_connect_bssid")
    with tempfile.TemporaryDirectory(prefix="wifi-ind-test-") as tmp:
        source, binary = Path(tmp)/"test.c", Path(tmp)/"test"
        source.write_text("\n".join((MOCKS, events, bits, JOIN_MOCKS, handlers, dispatcher, joins, TESTS)))
        subprocess.run(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-Wno-unused-function",
                        "-fsanitize=address,undefined", "-g", str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
