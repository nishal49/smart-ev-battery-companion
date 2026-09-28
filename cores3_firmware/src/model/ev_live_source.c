#include "ev_live_source.h"
#include "ev_ocv_table.h"

#include <math.h>
#include <string.h>

/* Bench pack: 3S (EV_LIVE_PACK_CELLS_S in the header, shared with the estimator
 * config). Range figures reuse the app's efficiency assumptions so the dashboard
 * stays coherent, just at the real measured scale. */
#define LIVE_CELLS_S            EV_LIVE_PACK_CELLS_S
#define LIVE_NOMINAL_WH_PER_KM  35.0f
#define LIVE_CONSERVATIVE_WH_PER_KM 42.0f
#define LIVE_RESERVE_FRACTION   0.90f
/* SoC = coulomb counting (backbone) anchored to OCV, but OCV is only trusted
 * when the pack is AT REST. This is how a production fuel gauge behaves: under
 * load the terminal voltage sags (IR + diffusion) and reads as false low SoC,
 * and it recovers on disconnect — so a voltage-influenced estimate would bounce.
 * We therefore:
 *   - integrate current continuously (immune to voltage sag), and
 *   - pull toward OCV only while |I| is below REST_CURRENT_A and has been for
 *     REST_SETTLE_MS (voltage has relaxed), using IR-compensated OCV. */
#define LIVE_OCV_BLEND          0.02f    /* correction strength, rest only */
#define LIVE_REST_CURRENT_A     0.15f    /* |I| below this = "resting" */
#define LIVE_REST_SETTLE_MS     8000U    /* rest this long before trusting OCV */
#define LIVE_CELL_R0_OHM        0.072f   /* per-cell R0 (CALCE fit ~71.7 mOhm) */

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Invert the per-cell OCV table to an SoC fraction (0..1). */
static float soc_from_cell_ocv(float cell_v)
{
    if (cell_v <= ev_ocv_table_v[0]) return 0.0f;
    if (cell_v >= ev_ocv_table_v[EV_OCV_POINTS - 1]) return 1.0f;
    for (int i = 1; i < EV_OCV_POINTS; ++i) {
        if (cell_v <= ev_ocv_table_v[i]) {
            const float span = ev_ocv_table_v[i] - ev_ocv_table_v[i - 1];
            const float frac = span > 1e-6f ? (cell_v - ev_ocv_table_v[i - 1]) / span : 0.0f;
            return ((float)(i - 1) + frac) / (float)(EV_OCV_POINTS - 1);
        }
    }
    return 1.0f;
}

void ev_live_source_init(ev_live_source_t * live, float capacity_ah)
{
    if (live == NULL) return;
    memset(live, 0, sizeof(*live));
    live->capacity_ah = capacity_ah > 0.1f ? capacity_ah : 2.5f;
    live->initialised = false;
}

bool ev_live_source_update(ev_live_source_t * live,
                           const ev_sensor_reading_t * reading,
                           ev_battery_state_t * out_state)
{
    if (live == NULL || reading == NULL || out_state == NULL) return false;
    if (!reading->present || !reading->valid) return false;

    const float pack_v = reading->pack_v;
    const float current_a = reading->current_a;
    const float cell_avg_v = pack_v / (float)LIVE_CELLS_S;

    /* IR-compensated open-circuit voltage: the terminal voltage sags under load
     * by I*R0 (discharge) or rises (charge). Adding I*R0 back recovers the true
     * OCV the SoC table expects. Sign: current_a>0 = charging here, so charging
     * lifts terminal V above OCV -> subtract; discharging -> add. Net: OCV =
     * V_terminal - I*R0 per cell. */
    const float cell_ocv_v = cell_avg_v - current_a * LIVE_CELL_R0_OHM;
    const float ocv_soc = soc_from_cell_ocv(cell_ocv_v) * 100.0f;

    /* Rest detection: OCV is only trustworthy once the pack has sat near zero
     * current long enough for the voltage to relax. Under load we integrate
     * only, which is why SoC no longer bounces back when the load is removed. */
    const bool near_zero = (current_a < LIVE_REST_CURRENT_A) &&
                           (current_a > -LIVE_REST_CURRENT_A);
    if (near_zero) {
        if (live->rest_since_ms == 0U) live->rest_since_ms = reading->sample_ms;
        if ((reading->sample_ms - live->rest_since_ms) >= LIVE_REST_SETTLE_MS) {
            live->resting = true;
        }
    } else {
        live->rest_since_ms = 0U;
        live->resting = false;
    }

    if (!live->initialised) {
        /* Seed from OCV on the first reading (assumed roughly at rest at boot). */
        live->soc_pct = ocv_soc;
        live->last_ms = reading->sample_ms;
        live->session_start_ms = reading->sample_ms;
        live->initialised = true;
    } else {
        const float dt_s = (float)(reading->sample_ms - live->last_ms) / 1000.0f;
        live->last_ms = reading->sample_ms;
        /* Coulomb counting is the backbone: SoC changes only by real amp-hours,
         * immune to voltage sag. This runs whether resting or under load. */
        if (dt_s > 0.0f && dt_s < 30.0f) {
            const float delta_ah = current_a * dt_s / 3600.0f;
            live->soc_pct += delta_ah * 100.0f / live->capacity_ah;
            /* Session energy (measured, not simulated): integrate |power| * dt.
             * current_a<0 = discharge (energy out), >0 = charge (energy in). */
            const float wh = pack_v * current_a * dt_s / 3600.0f;  /* signed Wh */
            if (wh < 0.0f) live->session_wh_out += -wh;
            else           live->session_wh_in  += wh;
            /* Track peak instantaneous |power| this session. */
            const float pw = pack_v * (current_a < 0.0f ? -current_a : current_a);
            if (pw > live->peak_power_w) live->peak_power_w = pw;
        }
        /* Anchor to OCV ONLY while resting, to remove integration drift without
         * chasing load-induced voltage sag/recovery. */
        if (live->resting) {
            live->soc_pct += (ocv_soc - live->soc_pct) * LIVE_OCV_BLEND;
        }
        live->soc_pct = clampf(live->soc_pct, 0.0f, 100.0f);
    }

    memset(out_state, 0, sizeof(*out_state));
    out_state->source = EV_DATA_SOURCE_LIVE_BENCH;
    /* Detect charge vs discharge from the measured current sign (convention:
     * positive = into the pack = charging). The threshold must sit ABOVE the
     * real idle offset: with no load the ACS723 zero-point can drift ~0.3-0.5 A,
     * so a 0.2 A threshold falsely reported "charging" at idle. A charger pushes
     * a clear, steady positive current well above this, so 0.6 A cleanly
     * separates real charging from idle drift. Anything below reads as
     * discharge/idle, matching the dashboard's current deadband behaviour. */
    {
        const float CHARGE_DETECT_A = 0.60f;  /* above idle offset drift */
        if (current_a > CHARGE_DETECT_A) {
            out_state->scenario = EV_SCENARIO_CHARGING;
        } else {
            out_state->scenario = EV_SCENARIO_NORMAL_DISCHARGE;
        }
    }
    out_state->sensor_data_valid = true;
    out_state->cell_data_available = true;
    out_state->health_baseline_ready = false;

    out_state->soc_pct = live->soc_pct;
    out_state->voltage_v = pack_v;
    out_state->current_a = current_a;

    /* Session energy this power-up (real, integrated). trip_energy_kwh carries
     * the discharged energy; distance/efficiency stay -1 (sentinel = "no
     * odometer") so the UI shows n/a rather than a fabricated distance. The
     * weekly chart is left zeroed for live (no multi-day history on the bench). */
    out_state->trip_energy_kwh = live->session_wh_out / 1000.0f;
    out_state->trip_distance_km = -1.0f;      /* no distance sensor */
    out_state->efficiency_wh_per_km = -1.0f;  /* undefined without distance */

    /* Live session monitor fields (real, measured this power-up). */
    out_state->session_wh_out = live->session_wh_out;
    out_state->session_wh_in = live->session_wh_in;
    out_state->session_peak_power_w = live->peak_power_w;
    out_state->session_seconds =
        (reading->sample_ms - live->session_start_ms) / 1000U;
    /* Honest "no sensor" sentinel when no NTC is connected, rather than a fake
     * default. The UI renders this as "--". */
    out_state->temperature_c = reading->ntc_count > 0U
        ? reading->temperature_c[0] : EV_TEMPERATURE_UNKNOWN;
    out_state->power_kw = pack_v * current_a / 1000.0f;

    const float rated_energy_kwh =
        (float)LIVE_CELLS_S * 3.7f * live->capacity_ah / 1000.0f;
    out_state->remaining_energy_kwh = rated_energy_kwh * live->soc_pct / 100.0f;
    out_state->nominal_range_km =
        out_state->remaining_energy_kwh * 1000.0f / LIVE_NOMINAL_WH_PER_KM;
    out_state->conservative_range_km =
        out_state->remaining_energy_kwh * LIVE_RESERVE_FRACTION * 1000.0f /
        LIVE_CONSERVATIVE_WH_PER_KM;

    /* Quality: warn on hot/low. Skip the thermal test when no NTC is present so
     * a missing sensor never trips a false thermal warning. */
    const bool have_temp = out_state->temperature_c > EV_TEMPERATURE_UNKNOWN + 1.0f;
    if ((have_temp && out_state->temperature_c >= 45.0f) || out_state->soc_pct <= 15.0f) {
        out_state->quality = EV_DATA_QUALITY_WARNING;
    } else {
        out_state->quality = EV_DATA_QUALITY_GOOD;
    }
    return true;
}
