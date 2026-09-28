/* On-device port of the Task 3 EKF (analysis/estimators.py). Same OCV
 * table, R0/R1/C1, and process/measurement noise. Adapted from a single
 * INR18650-20R cell to the 13S pack by scaling the OCV by 13 and using
 * a pack-level R0. */

#include "ev_estimator.h"
#include "ev_ocv_table.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

/* Default pack geometry (13S/30 Ah sim pack). Live use overrides via
 * ev_estimator_configure_pack(). */
#define EV_ESTIMATOR_DEFAULT_CELLS_S    13U
#define EV_ESTIMATOR_DEFAULT_CAP_AH     30.0f
/* Per-CELL RC parameters from the Task 3 CALCE fit; pack values scale with the
 * configured cell count so the model works for any series count. */
#define EV_ESTIMATOR_R0_OHM_CELL        0.0717f      /* CALCE fit 71.7 mOhm */
#define EV_ESTIMATOR_R1_OHM_CELL        0.0635f      /* CALCE fit 63.5 mOhm */
#define EV_ESTIMATOR_C1_FARAD           1124.0f      /* CALCE fit (tau ~71 s) */
#define EV_ESTIMATOR_Q_SOC              1.0e-10f
#define EV_ESTIMATOR_Q_VRC              1.0e-6f
#define EV_ESTIMATOR_R_VOLT             1.0e-4f
#define EV_ESTIMATOR_DEMO_SEED_PP       10.0f
#define EV_ESTIMATOR_READY_P_SOC        1.0e-3f      /* covariance to declare READY */
#define EV_ESTIMATOR_READY_UPDATES      6U
#define EV_ESTIMATOR_TEMP_LOW_C         10.0f
#define EV_ESTIMATOR_TEMP_HIGH_C        50.0f

/* One-time storage for the shared OCV curve (declared extern in the header). */
const float ev_ocv_table_v[EV_OCV_POINTS] = {
    3.0123f, 3.4416f, 3.4979f, 3.5444f, 3.5737f,
    3.5916f, 3.6079f, 3.6245f, 3.6433f, 3.6666f,
    3.6949f, 3.7263f, 3.7641f, 3.8090f, 3.8599f,
    3.9148f, 3.9721f, 4.0318f, 4.0695f, 4.1084f,
    4.1960f,
};

static float clampf_local(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Linear interpolation on the 21-point cell OCV table. soc is 0..1. */
static float cell_ocv_from_soc(float soc)
{
    const float clamped = clampf_local(soc, 0.0f, 1.0f);
    const float scaled = clamped * (float)(EV_OCV_POINTS - 1);
    const int index = (int)scaled;
    if (index >= EV_OCV_POINTS - 1) return ev_ocv_table_v[EV_OCV_POINTS - 1];
    const float frac = scaled - (float)index;
    return ev_ocv_table_v[index] * (1.0f - frac) + ev_ocv_table_v[index + 1] * frac;
}

/* Local dOCV/dSoC per cell (per unit SoC), used as the measurement Jacobian. */
static float cell_docv_dsoc(float soc)
{
    const float clamped = clampf_local(soc, 0.0f, 1.0f);
    const float scaled = clamped * (float)(EV_OCV_POINTS - 1);
    int index = (int)scaled;
    if (index >= EV_OCV_POINTS - 1) index = EV_OCV_POINTS - 2;
    /* Each table step spans 1/(EV_OCV_POINTS-1) of SoC. */
    return (ev_ocv_table_v[index + 1] - ev_ocv_table_v[index]) * (float)(EV_OCV_POINTS - 1);
}

/* Terminal pack voltage predicted by the RC model at (SoC, i, v_rc), for a pack
 * of `cells_s` series cells. R0 scales with cell count (per-cell R0 in series). */
static float pack_v_predict(float soc, float pack_current_a, float v_rc,
                            uint8_t cells_s)
{
    const float ocv_pack = cell_ocv_from_soc(soc) * (float)cells_s;
    const float r0_pack = EV_ESTIMATOR_R0_OHM_CELL * (float)cells_s;
    return ocv_pack + pack_current_a * r0_pack + v_rc;
}

void ev_estimator_init(ev_estimator_t * estimator, float truth_soc_pct)
{
    if (estimator == NULL) return;
    memset(estimator, 0, sizeof(*estimator));

    float seed_pct;
    if (truth_soc_pct >= 0.0f) {
        seed_pct = clampf_local(truth_soc_pct + EV_ESTIMATOR_DEMO_SEED_PP, 0.0f, 100.0f);
    } else {
        seed_pct = 50.0f;
    }
    estimator->x_soc = seed_pct * 0.01f;
    estimator->x_vrc = 0.0f;
    /* Wide initial uncertainty so the first measurement pulls hard. */
    estimator->p_soc = 1.0e-2f;
    estimator->p_vrc = 1.0e-4f;
    estimator->initial_seed_pct = seed_pct;
    /* Default to the 13S/30 Ah sim pack; the live path overrides via
     * ev_estimator_configure_pack(). memset(0) above would leave cells_s=0,
     * which the step function treats as the default, but set it explicitly. */
    estimator->cells_s = EV_ESTIMATOR_DEFAULT_CELLS_S;
    estimator->capacity_ah = EV_ESTIMATOR_DEFAULT_CAP_AH;
    estimator->initialised = true;
}

void ev_estimator_configure_pack(ev_estimator_t * estimator,
                                 uint8_t cells_s, float capacity_ah)
{
    if (estimator == NULL) return;
    if (cells_s > 0U) estimator->cells_s = cells_s;
    if (capacity_ah > 0.1f) estimator->capacity_ah = capacity_ah;
}

void ev_estimator_step(ev_estimator_t * estimator,
                       const ev_battery_state_t * measured,
                       float simulated_dt_s)
{
    if (estimator == NULL || measured == NULL) return;
    if (!estimator->initialised) return;
    if (!measured->sensor_data_valid || simulated_dt_s <= 0.0f) {
        estimator->last_residual_v_pack = 0.0f;
        return;
    }

    const uint8_t cells_s = estimator->cells_s > 0U ? estimator->cells_s
                                                    : EV_ESTIMATOR_DEFAULT_CELLS_S;
    const float cap_ah = estimator->capacity_ah > 0.1f ? estimator->capacity_ah
                                                        : EV_ESTIMATOR_DEFAULT_CAP_AH;
    /* R1 scales with series cell count (per-cell R1 in series). */
    const float r1_pack = EV_ESTIMATOR_R1_OHM_CELL * (float)cells_s;

    const float dt = simulated_dt_s;
    const float tau = r1_pack * EV_ESTIMATOR_C1_FARAD;
    const float decay = expf(-dt / (tau > 1e-3f ? tau : 1e-3f));
    const float capacity_as = cap_ah * 3600.0f;
    const float i_a = measured->current_a;

    /* --- Predict --- */
    float x_soc = estimator->x_soc + i_a * dt / capacity_as;
    float x_vrc = estimator->x_vrc * decay + r1_pack * (1.0f - decay) * i_a;

    /* F = diag(1, decay), so the covariance predicts as: */
    float p_soc = estimator->p_soc + EV_ESTIMATOR_Q_SOC;
    float p_vrc = estimator->p_vrc * decay * decay + EV_ESTIMATOR_Q_VRC;

    /* --- Update against pack terminal voltage --- */
    const float v_predict = pack_v_predict(x_soc, i_a, x_vrc, cells_s);
    const float h_soc = cell_docv_dsoc(x_soc) * (float)cells_s;
    const float h_vrc = 1.0f;
    const float s = h_soc * h_soc * p_soc + h_vrc * h_vrc * p_vrc + EV_ESTIMATOR_R_VOLT;
    const float k_soc = (p_soc * h_soc) / s;
    const float k_vrc = (p_vrc * h_vrc) / s;
    const float residual = measured->voltage_v - v_predict;

    x_soc += k_soc * residual;
    x_vrc += k_vrc * residual;
    p_soc *= (1.0f - k_soc * h_soc);
    p_vrc *= (1.0f - k_vrc * h_vrc);

    estimator->x_soc = clampf_local(x_soc, 0.0f, 1.0f);
    estimator->x_vrc = x_vrc;
    estimator->p_soc = p_soc > 0.0f ? p_soc : EV_ESTIMATOR_Q_SOC;
    estimator->p_vrc = p_vrc > 0.0f ? p_vrc : EV_ESTIMATOR_Q_VRC;
    estimator->last_residual_v_pack = residual;
    estimator->updates++;
}

void ev_estimator_get_snapshot(const ev_estimator_t * estimator,
                               ev_estimator_snapshot_t * snapshot)
{
    if (snapshot == NULL) return;
    memset(snapshot, 0, sizeof(*snapshot));
    if (estimator == NULL || !estimator->initialised) {
        snapshot->state = EV_ESTIMATOR_UNAVAILABLE;
        snapshot->method = "OCV+EKF";
        return;
    }

    snapshot->soc_pct = estimator->x_soc * 100.0f;
    snapshot->p_soc = estimator->p_soc;
    snapshot->residual_v_pack = estimator->last_residual_v_pack;
    snapshot->updates = estimator->updates;
    snapshot->initial_seed_pct = estimator->initial_seed_pct;
    snapshot->method = "OCV+EKF";

    const bool converged = estimator->p_soc <= EV_ESTIMATOR_READY_P_SOC &&
                           estimator->updates >= EV_ESTIMATOR_READY_UPDATES;
    if (!converged) {
        /* Confidence grows linearly with updates during learning. */
        const float ramp = (float)estimator->updates /
                           (float)(EV_ESTIMATOR_READY_UPDATES > 0U ? EV_ESTIMATOR_READY_UPDATES : 1U);
        snapshot->state = EV_ESTIMATOR_LEARNING;
        snapshot->confidence = clampf_local(ramp * 0.4f, 0.0f, 0.4f);
    } else {
        snapshot->state = EV_ESTIMATOR_READY;
        snapshot->confidence = 0.95f;
    }
}

const char * ev_estimator_state_name(ev_estimator_state_t state)
{
    switch (state) {
        case EV_ESTIMATOR_LEARNING: return "LEARNING";
        case EV_ESTIMATOR_READY:    return "READY";
        case EV_ESTIMATOR_DERATED:  return "DERATED";
        case EV_ESTIMATOR_UNAVAILABLE:
        default:                    return "OFFLINE";
    }
}

/* --- Temperature/derating hook -----------------------------------------
 * The Task 3 study showed the EKF degrades at 0 C (parameters fitted at
 * 25 C only). Anything outside a comfortable envelope is flagged as
 * DERATED so the UI can show reduced confidence rather than a bare
 * number pretending to be accurate. Called from the getter to keep the
 * gating logic in one place. */
static void apply_temperature_derating(ev_estimator_snapshot_t * snapshot,
                                       const ev_battery_state_t * measured)
{
    if (snapshot == NULL || measured == NULL) return;
    if (snapshot->state != EV_ESTIMATOR_READY) return;
    if (measured->temperature_c < EV_ESTIMATOR_TEMP_LOW_C ||
        measured->temperature_c > EV_ESTIMATOR_TEMP_HIGH_C) {
        snapshot->state = EV_ESTIMATOR_DERATED;
        snapshot->confidence = 0.55f;
    }
}

/* Public helper: refine the snapshot with battery-state gating. */
void ev_estimator_get_snapshot_with_state(const ev_estimator_t * estimator,
                                          const ev_battery_state_t * measured,
                                          ev_estimator_snapshot_t * snapshot)
{
    ev_estimator_get_snapshot(estimator, snapshot);
    if (measured == NULL || !measured->sensor_data_valid) {
        if (snapshot != NULL) {
            snapshot->state = EV_ESTIMATOR_UNAVAILABLE;
            snapshot->confidence = 0.0f;
        }
        return;
    }
    apply_temperature_derating(snapshot, measured);
}
