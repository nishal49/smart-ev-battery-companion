#include "ev_battery_model.h"
#include "ev_ocv_table.h"

#include <stddef.h>
#include <string.h>

#define SIMULATION_SPEED 6.0f
#define NMC_PACK_MIN_V 39.0f
#define NMC_PACK_MAX_V 54.6f
#define NMC_PACK_CELLS_S 13

static float clampf(float value, float minimum, float maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static float absf_local(float value)
{
    return value < 0.0f ? -value : value;
}

static float scenario_current_a(ev_scenario_t scenario, uint32_t tick)
{
    static const float normal_pattern[] = {-9.0f, -11.5f, -14.0f, -17.0f, -12.5f, -10.0f};
    static const float charge_pattern[] = {8.0f, 8.5f, 9.0f, 8.5f};
    static const float high_pattern[] = {-25.0f, -29.0f, -34.0f, -31.0f, -27.0f};

    switch (scenario) {
        case EV_SCENARIO_CHARGING:
            return charge_pattern[tick % (sizeof(charge_pattern) / sizeof(charge_pattern[0]))];
        case EV_SCENARIO_HIGH_CURRENT:
            return high_pattern[tick % (sizeof(high_pattern) / sizeof(high_pattern[0]))];
        case EV_SCENARIO_THERMAL_WARNING:
            return -13.0f - (float)(tick % 3U);
        case EV_SCENARIO_LOW_SOC:
            return -6.0f - (float)(tick % 4U);
        case EV_SCENARIO_SENSOR_FAULT:
            return 0.0f;
        case EV_SCENARIO_NORMAL_DISCHARGE:
        default:
            return normal_pattern[tick % (sizeof(normal_pattern) / sizeof(normal_pattern[0]))];
    }
}

static float scenario_target_temperature_c(ev_scenario_t scenario, float current_a)
{
    switch (scenario) {
        case EV_SCENARIO_CHARGING: return 32.0f;
        case EV_SCENARIO_HIGH_CURRENT: return 43.0f;
        case EV_SCENARIO_THERMAL_WARNING: return 53.0f;
        case EV_SCENARIO_LOW_SOC: return 34.0f;
        case EV_SCENARIO_SENSOR_FAULT: return 0.0f;
        case EV_SCENARIO_NORMAL_DISCHARGE:
        default: return 30.0f + absf_local(current_a) * 0.16f;
    }
}

static void update_derived_values(ev_battery_model_t * model)
{
    ev_battery_state_t * state = &model->state;

    if (!state->sensor_data_valid) {
        state->voltage_v = 0.0f;
        state->current_a = 0.0f;
        state->temperature_c = 0.0f;
        state->power_kw = 0.0f;
        state->remaining_energy_kwh = 0.0f;
        state->nominal_range_km = 0.0f;
        state->conservative_range_km = 0.0f;
        return;
    }

    /* Use the CALCE INR18650-20R OCV table for the per-cell curve so the
     * simulator and the on-device estimator agree on chemistry. Linear
     * interpolation between the 21 tabulated points. */
    const float soc_fraction = clampf(state->soc_pct * 0.01f, 0.0f, 1.0f);
    const float scaled = soc_fraction * (float)(EV_OCV_POINTS - 1);
    int i0 = (int)scaled;
    if (i0 >= EV_OCV_POINTS - 1) i0 = EV_OCV_POINTS - 2;
    const float frac = scaled - (float)i0;
    const float cell_ocv = ev_ocv_table_v[i0] * (1.0f - frac) + ev_ocv_table_v[i0 + 1] * frac;
    const float pack_ocv = cell_ocv * (float)NMC_PACK_CELLS_S;

    state->voltage_v = clampf(
        pack_ocv + state->current_a * model->internal_resistance_ohm,
        NMC_PACK_MIN_V - 1.0f,
        NMC_PACK_MAX_V
    );
    state->power_kw = state->voltage_v * state->current_a / 1000.0f;

    const float rated_energy_kwh = model->nominal_voltage_v * model->capacity_ah / 1000.0f;
    state->remaining_energy_kwh = rated_energy_kwh * state->soc_pct / 100.0f;
    state->nominal_range_km = state->remaining_energy_kwh * 1000.0f / model->nominal_efficiency_wh_per_km;
    state->conservative_range_km = state->remaining_energy_kwh * 0.90f * 1000.0f /
                                  model->conservative_efficiency_wh_per_km;
}

void ev_battery_model_set_scenario(ev_battery_model_t * model, ev_scenario_t scenario)
{
    if (model == NULL) return;
    if (scenario >= EV_SCENARIO_COUNT) scenario = EV_SCENARIO_NORMAL_DISCHARGE;

    ev_battery_state_t * state = &model->state;
    memset(state, 0, sizeof(*state));

    state->source = scenario == EV_SCENARIO_SENSOR_FAULT ?
                    EV_DATA_SOURCE_FAULT_TEST : EV_DATA_SOURCE_SIMULATED;
    state->quality = EV_DATA_QUALITY_GOOD;
    state->scenario = scenario;
    state->sensor_data_valid = true;
    state->health_baseline_ready = false;
    state->cell_data_available = false;
    state->efficiency_wh_per_km = model->nominal_efficiency_wh_per_km;

    state->weekly_energy_kwh[0] = 3.2f;
    state->weekly_energy_kwh[1] = 4.1f;
    state->weekly_energy_kwh[2] = 2.7f;
    state->weekly_energy_kwh[3] = 5.0f;
    state->weekly_energy_kwh[4] = 3.8f;
    state->weekly_energy_kwh[5] = 4.4f;
    state->weekly_energy_kwh[6] = 0.0f;

    switch (scenario) {
        case EV_SCENARIO_CHARGING:
            state->soc_pct = 45.0f;
            state->temperature_c = 30.0f;
            break;
        case EV_SCENARIO_HIGH_CURRENT:
            state->soc_pct = 65.0f;
            state->temperature_c = 36.0f;
            break;
        case EV_SCENARIO_THERMAL_WARNING:
            state->soc_pct = 58.0f;
            state->temperature_c = 47.0f;
            state->quality = EV_DATA_QUALITY_WARNING;
            break;
        case EV_SCENARIO_LOW_SOC:
            state->soc_pct = 12.0f;
            state->temperature_c = 33.0f;
            state->quality = EV_DATA_QUALITY_WARNING;
            break;
        case EV_SCENARIO_SENSOR_FAULT:
            state->soc_pct = 0.0f;
            state->sensor_data_valid = false;
            state->quality = EV_DATA_QUALITY_FAULT;
            break;
        case EV_SCENARIO_NORMAL_DISCHARGE:
        default:
            state->soc_pct = 78.0f;
            state->temperature_c = 31.0f;
            break;
    }

    state->current_a = scenario_current_a(scenario, 0U);
    update_derived_values(model);
}

void ev_battery_model_init(ev_battery_model_t * model)
{
    if (model == NULL) return;
    memset(model, 0, sizeof(*model));

    model->capacity_ah = 30.0f;
    model->nominal_voltage_v = 48.0f;
    model->internal_resistance_ohm = 0.060f;
    model->nominal_efficiency_wh_per_km = 35.0f;
    model->conservative_efficiency_wh_per_km = 42.0f;

    ev_battery_model_set_scenario(model, EV_SCENARIO_NORMAL_DISCHARGE);
}

void ev_battery_model_step(ev_battery_model_t * model, uint32_t real_elapsed_ms)
{
    if (model == NULL || real_elapsed_ms == 0U) return;

    ev_battery_state_t * state = &model->state;
    const float simulated_elapsed_s = ((float)real_elapsed_ms / 1000.0f) * SIMULATION_SPEED;
    state->simulated_time_s += (uint32_t)(simulated_elapsed_s + 0.5f);
    state->sequence++;

    if (!state->sensor_data_valid) {
        update_derived_values(model);
        return;
    }

    const uint32_t pattern_tick = state->sequence;
    state->current_a = scenario_current_a(state->scenario, pattern_tick);

    const float delta_ah = state->current_a * simulated_elapsed_s / 3600.0f;
    state->soc_pct = clampf(state->soc_pct + delta_ah * 100.0f / model->capacity_ah, 0.0f, 100.0f);

    const float target_temperature = scenario_target_temperature_c(state->scenario, state->current_a);
    state->temperature_c += (target_temperature - state->temperature_c) * 0.10f;

    update_derived_values(model);

    if (state->current_a < 0.0f) {
        const float consumed_kwh = (-state->power_kw) * simulated_elapsed_s / 3600.0f;
        state->trip_energy_kwh += consumed_kwh;
        state->trip_distance_km += consumed_kwh * 1000.0f / model->nominal_efficiency_wh_per_km;
        state->weekly_energy_kwh[6] = state->trip_energy_kwh;
    }

    if (state->trip_distance_km > 0.001f) {
        state->efficiency_wh_per_km = state->trip_energy_kwh * 1000.0f / state->trip_distance_km;
    }

    if (state->temperature_c >= 45.0f || state->soc_pct <= 15.0f) {
        state->quality = EV_DATA_QUALITY_WARNING;
    } else {
        state->quality = EV_DATA_QUALITY_GOOD;
    }
}

const ev_battery_state_t * ev_battery_model_get_state(const ev_battery_model_t * model)
{
    return model == NULL ? NULL : &model->state;
}

const char * ev_data_source_name(ev_data_source_t source)
{
    switch (source) {
        case EV_DATA_SOURCE_DATASET: return "DATASET";
        case EV_DATA_SOURCE_LIVE_BENCH: return "LIVE";
        case EV_DATA_SOURCE_FAULT_TEST: return "FAULT TEST";
        case EV_DATA_SOURCE_SIMULATED:
        default: return "SIM";
    }
}

const char * ev_data_quality_name(ev_data_quality_t quality)
{
    switch (quality) {
        case EV_DATA_QUALITY_WARNING: return "WARNING";
        case EV_DATA_QUALITY_FAULT: return "FAULT";
        case EV_DATA_QUALITY_GOOD:
        default: return "GOOD";
    }
}

const char * ev_scenario_name(ev_scenario_t scenario)
{
    switch (scenario) {
        case EV_SCENARIO_CHARGING: return "CHARGING";
        case EV_SCENARIO_HIGH_CURRENT: return "HIGH LOAD";
        case EV_SCENARIO_THERMAL_WARNING: return "THERMAL";
        case EV_SCENARIO_LOW_SOC: return "LOW SOC";
        case EV_SCENARIO_SENSOR_FAULT: return "SENSOR FAULT";
        case EV_SCENARIO_NORMAL_DISCHARGE:
        default: return "NORMAL";
    }
}
