#ifndef CARBOX_HTTP_OWNER_DIAG_H
#define CARBOX_HTTP_OWNER_DIAG_H
#include <stdint.h>
#if defined(CONFIG_MOVE_LAG_PROFILE) && CONFIG_MOVE_LAG_PROFILE
void http_owner_enqueue(void *client, void *message, int bypass, uintptr_t caller);
void http_owner_enqueue_result(void *message, int32_t result);
void http_owner_write_begin(void *message, void *context);
void http_owner_write_end(void *message, int32_t result);
void http_owner_drain(void *client, const void *data, uint32_t length, int32_t result);
void http_owner_guard(void *client);
int32_t http_owner_vendor_read(void *message, void *read_f, void *context);
void http_owner_report(uint32_t sequence);
#else
#define http_owner_enqueue(...) ((void)0)
#define http_owner_enqueue_result(...) ((void)0)
#define http_owner_write_begin(...) ((void)0)
#define http_owner_write_end(...) ((void)0)
#define http_owner_drain(...) ((void)0)
#define http_owner_guard(...) ((void)0)
#define http_owner_vendor_read(m,f,c) __real_HTTPMessageReadMessage(m,f,c)
#define http_owner_report(...) ((void)0)
#endif
#endif
