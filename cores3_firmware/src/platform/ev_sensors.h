#ifndef EV_SENSORS_H
#define EV_SENSORS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Low-voltage bench sensor HAL: ADS1115 (3 cell taps + ACS723 current) on the
 * external I2C bus, plus two NTC thermistors on the ESP32-S3 internal ADC.
 *
 * Everything degrades gracefully: if the ADS1115 is not detected on I2C the
 * module reports present=false, and the app stays on the simulation. When the
 * hardware is wired the same code produces live readings with no other change.
 *
 * All calibration values are constants here for now (from the frozen design in
 * hardware/sensing-design.md); they will be replaced by measured values during
 * bench calibration. */

#define EV_SENSOR_CELLS 3U

typedef struct {
    bool  present;          /* ADS1115 detected on I2C this read */
    bool  valid;            /* readings are fresh and in range */

    float cell_v[EV_SENSOR_CELLS];   /* per-cell voltage (V) */
    float pack_v;                    /* sum of cells (V) */
    float current_a;                 /* pack current, + = charge (A) */
    float temperature_c[2];          /* NTC temperatures (C) */
    uint8_t ntc_count;               /* NTCs producing valid readings */

    uint32_t sample_ms;              /* millis() of this sample */
    uint32_t error_count;            /* cumulative I2C read failures */
} ev_sensor_reading_t;

/* Bring up the external I2C bus, probe the ADS1115, and start a background
 * FreeRTOS task that samples the sensors continuously and publishes a snapshot.
 * All blocking I2C happens on that task, never on the caller/LVGL thread. Safe
 * to call at startup even with nothing connected. Returns true if the task
 * started. */
bool ev_sensors_init(void);

/* Copy the latest published snapshot (non-blocking, mutex-guarded). This is
 * what the UI/loop calls every tick -- it never touches I2C. */
void ev_sensors_read(ev_sensor_reading_t * reading);

/* Capture the current-sensor zero offset. Call with NO load current flowing
 * (load switch open) so the ACS723 midpoint is learned. */
void ev_sensors_zero_current(void);

bool ev_sensors_present(void);

#ifdef __cplusplus
}
#endif

#endif /* EV_SENSORS_H */
