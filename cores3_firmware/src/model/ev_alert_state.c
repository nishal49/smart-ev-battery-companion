#include "ev_alert_state.h"

#include <string.h>

#define THERMAL_WARNING_ASSERT_C       45.0f
#define THERMAL_WARNING_CLEAR_C        42.0f
#define THERMAL_CRITICAL_ASSERT_C      50.0f
#define THERMAL_CRITICAL_DOWNGRADE_C   47.0f

#define LOW_SOC_WARNING_ASSERT_PCT     15.0f
#define LOW_SOC_WARNING_CLEAR_PCT      18.0f
#define LOW_SOC_CRITICAL_ASSERT_PCT     8.0f
#define LOW_SOC_CRITICAL_DOWNGRADE_PCT 10.0f

#define SENSOR_RECOVERY_SAMPLES         3U
#define WARNING_REMINDER_MS         30000U
#define CRITICAL_REMINDER_MS        15000U

static ev_alert_severity_t severity_for_kind(ev_alert_kind_t kind)
{
    switch (kind) {
        case EV_ALERT_SENSOR_FAULT:
            return EV_ALERT_SEVERITY_FAULT;
        case EV_ALERT_THERMAL_CRITICAL:
        case EV_ALERT_LOW_SOC_CRITICAL:
            return EV_ALERT_SEVERITY_CRITICAL;
        case EV_ALERT_THERMAL_WARNING:
        case EV_ALERT_LOW_SOC_WARNING:
            return EV_ALERT_SEVERITY_WARNING;
        case EV_ALERT_NONE:
        default:
            return EV_ALERT_SEVERITY_NONE;
    }
}

static ev_alert_kind_t highest_priority_alert(const ev_alert_state_t * alert_state)
{
    if (alert_state->sensor_fault_latched) return EV_ALERT_SENSOR_FAULT;
    if (alert_state->thermal_critical_latched) return EV_ALERT_THERMAL_CRITICAL;
    if (alert_state->low_soc_critical_latched) return EV_ALERT_LOW_SOC_CRITICAL;
    if (alert_state->thermal_warning_latched) return EV_ALERT_THERMAL_WARNING;
    if (alert_state->low_soc_warning_latched) return EV_ALERT_LOW_SOC_WARNING;
    return EV_ALERT_NONE;
}

static void update_sensor_fault(
    ev_alert_state_t * alert_state,
    bool sensor_data_valid,
    uint32_t real_elapsed_ms
)
{
    if (!sensor_data_valid) {
        alert_state->sensor_fault_latched = true;
        alert_state->valid_sensor_samples = 0U;
        return;
    }

    if (!alert_state->sensor_fault_latched) {
        alert_state->valid_sensor_samples = 0U;
        return;
    }

    /* Immediate scenario renders are not real recovery samples. */
    if (real_elapsed_ms < 1000U) return;

    if (alert_state->valid_sensor_samples < SENSOR_RECOVERY_SAMPLES) {
        alert_state->valid_sensor_samples++;
    }
    if (alert_state->valid_sensor_samples >= SENSOR_RECOVERY_SAMPLES) {
        alert_state->sensor_fault_latched = false;
        alert_state->valid_sensor_samples = 0U;
    }
}

static void update_thermal_latches(ev_alert_state_t * alert_state, float temperature_c)
{
    if (temperature_c >= THERMAL_CRITICAL_ASSERT_C) {
        alert_state->thermal_critical_latched = true;
    } else if (temperature_c < THERMAL_CRITICAL_DOWNGRADE_C) {
        alert_state->thermal_critical_latched = false;
    }

    if (temperature_c >= THERMAL_WARNING_ASSERT_C) {
        alert_state->thermal_warning_latched = true;
    } else if (temperature_c <= THERMAL_WARNING_CLEAR_C) {
        alert_state->thermal_warning_latched = false;
    }
}

static void update_low_soc_latches(ev_alert_state_t * alert_state, float soc_pct)
{
    if (soc_pct <= LOW_SOC_CRITICAL_ASSERT_PCT) {
        alert_state->low_soc_critical_latched = true;
    } else if (soc_pct > LOW_SOC_CRITICAL_DOWNGRADE_PCT) {
        alert_state->low_soc_critical_latched = false;
    }

    if (soc_pct <= LOW_SOC_WARNING_ASSERT_PCT) {
        alert_state->low_soc_warning_latched = true;
    } else if (soc_pct >= LOW_SOC_WARNING_CLEAR_PCT) {
        alert_state->low_soc_warning_latched = false;
    }
}

void ev_alert_state_init(ev_alert_state_t * alert_state)
{
    if (alert_state == NULL) return;
    memset(alert_state, 0, sizeof(*alert_state));
}

void ev_alert_state_update(
    ev_alert_state_t * alert_state,
    const ev_battery_state_t * battery_state,
    uint32_t real_elapsed_ms
)
{
    if (alert_state == NULL || battery_state == NULL) return;

    const ev_alert_kind_t previous_kind = alert_state->snapshot.kind;
    alert_state->real_time_ms += real_elapsed_ms;
    alert_state->snapshot.audio_due = false;

    update_sensor_fault(alert_state, battery_state->sensor_data_valid, real_elapsed_ms);
    if (battery_state->sensor_data_valid) {
        update_thermal_latches(alert_state, battery_state->temperature_c);
        update_low_soc_latches(alert_state, battery_state->soc_pct);
    }

    const ev_alert_kind_t current_kind = highest_priority_alert(alert_state);
    const ev_alert_severity_t current_severity = severity_for_kind(current_kind);
    const bool active = current_kind != EV_ALERT_NONE;
    bool audio_due = active && current_kind != previous_kind;

    if (active && !audio_due && alert_state->has_audio_timestamp) {
        const uint32_t reminder_ms = current_severity >= EV_ALERT_SEVERITY_CRITICAL ?
                                     CRITICAL_REMINDER_MS : WARNING_REMINDER_MS;
        audio_due = (uint32_t)(alert_state->real_time_ms - alert_state->last_audio_ms) >= reminder_ms;
    }

    if (audio_due) {
        alert_state->last_audio_ms = alert_state->real_time_ms;
        alert_state->has_audio_timestamp = true;
    }

    alert_state->snapshot.active = active;
    alert_state->snapshot.audio_due = audio_due;
    alert_state->snapshot.kind = current_kind;
    alert_state->snapshot.severity = current_severity;
}

const ev_alert_snapshot_t * ev_alert_state_get_snapshot(const ev_alert_state_t * alert_state)
{
    return alert_state == NULL ? NULL : &alert_state->snapshot;
}
