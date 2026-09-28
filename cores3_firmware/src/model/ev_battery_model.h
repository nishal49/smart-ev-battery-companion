#ifndef EV_BATTERY_MODEL_H
#define EV_BATTERY_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EV_WEEK_DAYS 7

/* Sentinel for temperature_c when no thermistor is connected (physically
 * impossible value, so callers can render "--" instead of a fake reading). */
#define EV_TEMPERATURE_UNKNOWN (-273.0f)

typedef enum {
    EV_DATA_SOURCE_SIMULATED = 0,
    EV_DATA_SOURCE_DATASET,
    EV_DATA_SOURCE_LIVE_BENCH,
    EV_DATA_SOURCE_FAULT_TEST
} ev_data_source_t;

typedef enum {
    EV_DATA_QUALITY_GOOD = 0,
    EV_DATA_QUALITY_WARNING,
    EV_DATA_QUALITY_FAULT
} ev_data_quality_t;

typedef enum {
    EV_SCENARIO_NORMAL_DISCHARGE = 0,
    EV_SCENARIO_CHARGING,
    EV_SCENARIO_HIGH_CURRENT,
    EV_SCENARIO_THERMAL_WARNING,
    EV_SCENARIO_LOW_SOC,
    EV_SCENARIO_SENSOR_FAULT,
    EV_SCENARIO_COUNT
} ev_scenario_t;

typedef struct {
    uint32_t sequence;
    uint32_t simulated_time_s;
    ev_data_source_t source;
    ev_data_quality_t quality;
    ev_scenario_t scenario;
    bool sensor_data_valid;
    bool health_baseline_ready;
    bool cell_data_available;

    float soc_pct;
    float voltage_v;
    float current_a;
    float temperature_c;
    float power_kw;
    float remaining_energy_kwh;
    float nominal_range_km;
    float conservative_range_km;

    float trip_distance_km;
    float trip_energy_kwh;
    float efficiency_wh_per_km;
    float weekly_energy_kwh[EV_WEEK_DAYS];

    /* Live session monitor (populated by the live source; the sim leaves them 0).
     * These are real measured/integrated quantities since this power-up. */
    float session_wh_out;        /* energy discharged this session (Wh) */
    float session_wh_in;         /* energy charged this session (Wh) */
    float session_peak_power_w;  /* peak |pack power| seen this session (W) */
    uint32_t session_seconds;    /* elapsed session time (s) */
} ev_battery_state_t;

typedef struct {
    ev_battery_state_t state;
    float capacity_ah;
    float nominal_voltage_v;
    float internal_resistance_ohm;
    float nominal_efficiency_wh_per_km;
    float conservative_efficiency_wh_per_km;
} ev_battery_model_t;

void ev_battery_model_init(ev_battery_model_t * model);
void ev_battery_model_set_scenario(ev_battery_model_t * model, ev_scenario_t scenario);
void ev_battery_model_step(ev_battery_model_t * model, uint32_t real_elapsed_ms);
const ev_battery_state_t * ev_battery_model_get_state(const ev_battery_model_t * model);

const char * ev_data_source_name(ev_data_source_t source);
const char * ev_data_quality_name(ev_data_quality_t quality);
const char * ev_scenario_name(ev_scenario_t scenario);

#ifdef __cplusplus
}
#endif

#endif /* EV_BATTERY_MODEL_H */
