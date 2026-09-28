#ifndef EV_ALERT_STATE_H
#define EV_ALERT_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "ev_battery_model.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EV_ALERT_NONE = 0,
    EV_ALERT_LOW_SOC_WARNING,
    EV_ALERT_THERMAL_WARNING,
    EV_ALERT_LOW_SOC_CRITICAL,
    EV_ALERT_THERMAL_CRITICAL,
    EV_ALERT_SENSOR_FAULT
} ev_alert_kind_t;

typedef enum {
    EV_ALERT_SEVERITY_NONE = 0,
    EV_ALERT_SEVERITY_WARNING,
    EV_ALERT_SEVERITY_CRITICAL,
    EV_ALERT_SEVERITY_FAULT
} ev_alert_severity_t;

typedef struct {
    bool active;
    bool audio_due;
    ev_alert_kind_t kind;
    ev_alert_severity_t severity;
} ev_alert_snapshot_t;

typedef struct {
    bool sensor_fault_latched;
    bool thermal_warning_latched;
    bool thermal_critical_latched;
    bool low_soc_warning_latched;
    bool low_soc_critical_latched;
    uint8_t valid_sensor_samples;
    uint32_t real_time_ms;
    uint32_t last_audio_ms;
    bool has_audio_timestamp;
    ev_alert_snapshot_t snapshot;
} ev_alert_state_t;

void ev_alert_state_init(ev_alert_state_t * alert_state);
void ev_alert_state_update(
    ev_alert_state_t * alert_state,
    const ev_battery_state_t * battery_state,
    uint32_t real_elapsed_ms
);
const ev_alert_snapshot_t * ev_alert_state_get_snapshot(const ev_alert_state_t * alert_state);

#ifdef __cplusplus
}
#endif

#endif /* EV_ALERT_STATE_H */
