#ifndef CAR_RX_DIAG_H
#define CAR_RX_DIAG_H
#include <stdint.h>
enum { CAR_RX_READ, CAR_RX_RECV, CAR_RX_CONNECT, CAR_RX_SELECT, CAR_RX_HTTP, CAR_RX_TYPE, CAR_RX_RECVFROM };
#if CONFIG_MOVE_LAG_PROFILE
int car_rx_begin(int api, int fd);
void car_rx_end(int token, int result, int error);
void car_rx_type(const char *type, uint32_t bytes, int result);
void car_rx_report(uint32_t sequence);
#else
static inline int car_rx_begin(int api, int fd) { (void)api; (void)fd; return -1; }
static inline void car_rx_end(int token, int result, int error) { (void)token; (void)result; (void)error; }
static inline void car_rx_type(const char *type, uint32_t bytes, int result) { (void)type; (void)bytes; (void)result; }
static inline void car_rx_report(uint32_t sequence) { (void)sequence; }
#endif
#endif
