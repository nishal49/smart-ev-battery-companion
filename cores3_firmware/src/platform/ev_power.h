#ifndef EV_POWER_H
#define EV_POWER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Power management for the CoreS3-SE (AXP2101 PMIC + BM8563 RTC).
 *
 * Two hardware-backed behaviours a real vehicle-mounted companion needs:
 *
 *  1. Graceful shutdown: a long-press of the side POWER button flushes pending
 *     telemetry and powers the board down cleanly via the PMIC, instead of the
 *     user yanking USB power mid-write.
 *
 *  2. Idle power-saver: with no touch for a while the display dims, then the
 *     board enters light-sleep to cut draw, waking on the next touch. This
 *     matters when running from a power bank or a vehicle's accessory rail.
 *
 * Call ev_power_init() once after M5.begin(), then ev_power_service() every
 * loop iteration. Touch activity is detected from M5.Touch inside service().
 */

typedef struct {
    bool     saver_enabled;   /* master toggle for the idle power-saver */
    uint32_t dim_after_ms;    /* idle time before dimming the display */
    uint32_t sleep_after_ms;  /* idle time before light-sleep (>= dim_after_ms) */
    uint8_t  active_brightness;  /* 0..255 normal brightness */
    uint8_t  dim_brightness;     /* 0..255 dimmed brightness */
} ev_power_config_t;

void ev_power_init(uint8_t active_brightness);

/* Poll the power button and drive the idle state machine. Returns false while
 * normal, true on the tick it wakes from sleep (so the caller can refresh). */
void ev_power_service(void);

/* Enable/disable the idle power-saver at runtime (Settings toggle). */
void ev_power_set_saver_enabled(bool enabled);
bool ev_power_saver_enabled(void);

/* Set the normal (active) display brightness; also applies it immediately if
 * the display is currently active (not dimmed/asleep). */
void ev_power_set_active_brightness(uint8_t brightness);

/* Perform a graceful shutdown now: flush logs, then power off via the PMIC.
 * Does not return if the PMIC honours the request. */
void ev_power_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* EV_POWER_H */
