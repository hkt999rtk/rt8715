/* Temporary, observation-only screen receive liveness probe. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "diag.h"
#include "lwip/sockets.h"
#include "crypto_priority_lock.h"

extern ssize_t __real_lwip_recv(int socket, void *buffer, size_t bytes,
                                int flags);
extern size_t __real_chacha20_poly1305_decrypt(void *state, const void *src,
                                               size_t len, void *dst);
extern size_t __real_chacha20_poly1305_verify(void *state, void *dst,
                                              const uint8_t tag[16],
                                              int32_t *out_error);

typedef struct {
  volatile uint32_t rx_calls;
  volatile uint32_t rx_bytes;
  volatile uint32_t rx_zero;
  volatile uint32_t rx_error;
  volatile uint32_t decrypt_calls;
  volatile uint32_t verify_ok;
  volatile uint32_t verify_error;
  volatile TickType_t last_rx_tick;
  volatile TickType_t last_verify_tick;
  volatile TickType_t phase_tick;
  volatile int32_t last_verify_error;
  volatile int32_t last_recv_result;
  volatile int32_t last_socket;
  volatile uint32_t phase; /* 0=between calls, 1=recv, 2=decrypt, 3=verify */
} screen_stop_diag_t;

static screen_stop_diag_t g_screen_stop_diag
  __attribute__((section(".lpddr.bss.screen_stop_diag")));
static volatile uint32_t g_reporter_started;

static int screen_stop_is_receiver(void)
{
  TaskHandle_t task = xTaskGetCurrentTaskHandle();
  const char *name;
  if (task == NULL) return 0;
  name = pcTaskGetName(task);
  if (name == NULL || strcmp(name, "AirPlayScreenReceiver") != 0) return 0;
  return 1;
}

static void screen_stop_reporter(void *arg)
{
  screen_stop_diag_t previous = { 0 };
  uint32_t sequence = 0;
  (void)arg;
  for (;;) {
    screen_stop_diag_t current;
    carbox_crypto_irq_snapshot_t irq;
    TickType_t now;
    vTaskDelay(pdMS_TO_TICKS(10000));
    now = xTaskGetTickCount();
    current = g_screen_stop_diag;
    carbox_crypto_irq_controller_snapshot(&irq);
    rt_printf("[SCREENSTOP][%lu] rx=%lu bytes=%lu zero=%lu err=%lu "
              "dec=%lu verify_ok/err=%lu/%lu last_rx_age_ms=%lu "
              "last_verify_age_ms=%lu phase=%lu phase_age_ms=%lu "
              "last_recv=%ld fd=%ld verify_err=%ld irq_timeout/reset/gen=%lu/%lu/%lu\r\n",
              (unsigned long)++sequence,
              (unsigned long)(current.rx_calls - previous.rx_calls),
              (unsigned long)(current.rx_bytes - previous.rx_bytes),
              (unsigned long)(current.rx_zero - previous.rx_zero),
              (unsigned long)(current.rx_error - previous.rx_error),
              (unsigned long)(current.decrypt_calls - previous.decrypt_calls),
              (unsigned long)(current.verify_ok - previous.verify_ok),
              (unsigned long)(current.verify_error - previous.verify_error),
              current.last_rx_tick ?
                (unsigned long)((now - current.last_rx_tick) * portTICK_PERIOD_MS) : 0UL,
              current.last_verify_tick ?
                (unsigned long)((now - current.last_verify_tick) * portTICK_PERIOD_MS) : 0UL,
              (unsigned long)current.phase,
              current.phase ?
                (unsigned long)((now - current.phase_tick) * portTICK_PERIOD_MS) : 0UL,
              (long)current.last_recv_result, (long)current.last_socket,
              (long)current.last_verify_error,
              (unsigned long)irq.timeout_count,
              (unsigned long)irq.reset_count,
              (unsigned long)irq.generation);
    previous = current;
  }
}

static void screen_stop_start_reporter(void)
{
  if (g_reporter_started) return;
  if (!__sync_bool_compare_and_swap(&g_reporter_started, 0u, 1u)) return;
  if (xTaskCreate(screen_stop_reporter, "screenstop", 768u, NULL,
                  tskIDLE_PRIORITY + 1u, NULL) != pdPASS) {
    g_reporter_started = 2u;
    rt_printf("[SCREENSTOP] reporter task creation failed\r\n");
  } else {
    rt_printf("[SCREENSTOP] receive/crypto liveness probe active\r\n");
  }
}

ssize_t __wrap_lwip_recv(int socket, void *buffer, size_t bytes, int flags)
{
  ssize_t result;
  int tracked = screen_stop_is_receiver();
  if (tracked) {
    screen_stop_start_reporter();
    g_screen_stop_diag.last_socket = socket;
    g_screen_stop_diag.phase_tick = xTaskGetTickCount();
    g_screen_stop_diag.phase = 1u;
  }
  result = __real_lwip_recv(socket, buffer, bytes, flags);
  if (tracked) {
    g_screen_stop_diag.rx_calls++;
    g_screen_stop_diag.last_recv_result = (int32_t)result;
    if (result > 0) {
      g_screen_stop_diag.rx_bytes += (uint32_t)result;
      g_screen_stop_diag.last_rx_tick = xTaskGetTickCount();
    } else if (result == 0) {
      g_screen_stop_diag.rx_zero++;
    } else {
      g_screen_stop_diag.rx_error++;
    }
    g_screen_stop_diag.phase = 0u;
  }
  return result;
}

size_t __wrap_chacha20_poly1305_decrypt(void *state, const void *src,
                                         size_t len, void *dst)
{
  size_t written;
  int tracked = screen_stop_is_receiver();
  if (tracked) {
    g_screen_stop_diag.phase_tick = xTaskGetTickCount();
    g_screen_stop_diag.phase = 2u;
  }
  written = __real_chacha20_poly1305_decrypt(state, src, len, dst);
  if (tracked) {
    g_screen_stop_diag.decrypt_calls++;
    g_screen_stop_diag.phase = 0u;
  }
  return written;
}

size_t __wrap_chacha20_poly1305_verify(void *state, void *dst,
                                        const uint8_t tag[16],
                                        int32_t *out_error)
{
  size_t written;
  int tracked = screen_stop_is_receiver();
  if (tracked) {
    g_screen_stop_diag.phase_tick = xTaskGetTickCount();
    g_screen_stop_diag.phase = 3u;
  }
  written = __real_chacha20_poly1305_verify(state, dst, tag, out_error);
  if (tracked) {
    int32_t error = out_error ? *out_error : INT32_MIN;
    g_screen_stop_diag.last_verify_error = error;
    g_screen_stop_diag.last_verify_tick = xTaskGetTickCount();
    if (error == 0) g_screen_stop_diag.verify_ok++;
    else g_screen_stop_diag.verify_error++;
    g_screen_stop_diag.phase = 0u;
  }
  return written;
}
