/* Report the RTL8195B RF thermal meter in the existing SystemLib usage log.
 * The SDK exposes a trimmed 0..63 reading, not a calibrated Celsius value. */
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

extern int __real_CarApi_GetUsage(char *cpu, char *usage);
extern int rltk_wlan_get_thermal(uint8_t *value);

int __wrap_CarApi_GetUsage(char *cpu, char *usage)
{
  static TickType_t last_sample_tick;
  static uint8_t last_raw;
  static uint8_t valid;
  static const char placeholder[] = "Temp -10.0 C";
  static const char unavailable[] = "RF raw N/A  ";
  char *temperature;
  TickType_t now;
  int result = __real_CarApi_GetUsage(cpu, usage);

  if (!result || !usage) return result;
  temperature = strstr(usage, placeholder);
  if (!temperature) return result;

  now = xTaskGetTickCount();
  if (!valid || (TickType_t)(now - last_sample_tick) >= pdMS_TO_TICKS(10000)) {
    uint8_t raw = 0;
    valid = rltk_wlan_get_thermal(&raw) == 0 && raw <= 63u;
    if (valid) last_raw = raw;
    last_sample_tick = now;
  }

  if (valid) {
    /* Same 12-byte width as the original suffix; caller's buffer need not grow. */
    static const char format[] = "RF raw 00/63";
    memcpy(temperature, format, sizeof(format) - 1u);
    temperature[7] = (char)('0' + last_raw / 10u);
    temperature[8] = (char)('0' + last_raw % 10u);
  } else {
    memcpy(temperature, unavailable, sizeof(unavailable) - 1u);
  }
  return result;
}
