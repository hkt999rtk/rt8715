#ifndef WCAM_SENSOR_H
#define WCAM_SENSOR_H

#include <stdint.h>

int wcam_sensor_init(void);
void wcam_sensor_deinit(void);
/* Return any queued completions before waiting for the new link's first frame. */
void wcam_sensor_discard_ready(void);
int wcam_sensor_get_frame(uint8_t **frame, uint32_t timeout_ms);
void wcam_sensor_release_frame(uint8_t *frame);
uint32_t wcam_sensor_drops(void);

#endif
