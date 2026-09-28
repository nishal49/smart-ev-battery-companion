/* On-device SoC estimator: first-order RC EKF using the CALCE
 * INR18650-20R OCV curve and the R0/R1/C1 parameters fitted in Task 3.
 * This is the same algorithm validated in analysis/run_validation.py
 * (held-out MAE 2.08 pp; 0.38 pp @ 25 C).
 */

#ifndef EV_ESTIMATOR_H
#define EV_ESTIMATOR_H

#include <stdbool.h>
#include <stdint.h>

#include "ev_battery_model.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EV_ESTIMATOR_UNAVAILABLE = 0,  /* sensor invalid or not seeded */
    EV_ESTIMATOR_LEARNING,          /* converging, do not trust yet */
    EV_ESTIMATOR_READY,             /* nominal operating envelope */
    EV_ESTIMATOR_DERATED             /* out-of-envelope (cold, saturated, etc.) */
} ev_estimator_state_t;

typedef struct {
    ev_estimator_state_t state;
    float soc_pct;              /* posterior SoC estimate, 0..100 */
    float confidence;           /* 0..1, drops when uncovered or cold */
    float residual_v_pack;      /* last innovation, pack-scaled */
    float p_soc;                /* diagonal covariance, for LEARNING gate */
    uint32_t updates;           /* number of measurement updates applied */
    float initial_seed_pct;     /* SoC the estimator was seeded with */
    const char * method;        /* human-readable label */
} ev_estimator_snapshot_t;

typedef struct {
    float p_soc;
    float p_vrc;
    float x_soc;
    float x_vrc;
    uint32_t updates;
    float initial_seed_pct;
    float last_residual_v_pack;
    bool  initialised;
    /* Pack geometry the voltage model uses. Defaults to the 13S/30 Ah sim pack
     * (see ev_estimator_init); set via ev_estimator_configure_pack() for the
     * live 3S bench pack so the predicted terminal voltage matches reality. */
    uint8_t cells_s;
    float   capacity_ah;
} ev_estimator_t;

/* Seed the filter. If truth_soc_pct >= 0 the filter is deliberately biased by
 * EV_ESTIMATOR_DEMO_SEED_PP so the demo shows convergence; pass -1 for a
 * neutral 50 % start. */
void ev_estimator_init(ev_estimator_t * estimator, float truth_soc_pct);

/* Configure the pack geometry the voltage model assumes. Call after init when
 * running against a pack that is not the default 13S/30 Ah sim pack (e.g. the
 * live 3S bench pack: cells_s=3, capacity_ah=~2.5). Without this the predicted
 * terminal voltage is ~13x a cell and the filter rails a small pack to 0 %. */
void ev_estimator_configure_pack(ev_estimator_t * estimator,
                                 uint8_t cells_s, float capacity_ah);

/* One EKF step at the same cadence as ev_battery_model_step. The battery
 * state supplies pack voltage, pack current and temperature; simulated_dt_s
 * is the simulated-time step used by the model (6 s per real tick in the
 * current sim, or the true dt when running against sensors). */
void ev_estimator_step(ev_estimator_t * estimator,
                       const ev_battery_state_t * measured,
                       float simulated_dt_s);

void ev_estimator_get_snapshot(const ev_estimator_t * estimator,
                               ev_estimator_snapshot_t * snapshot);

/* Same as ev_estimator_get_snapshot but folds in temperature and sensor
 * validity so callers see a single derating-aware state. */
void ev_estimator_get_snapshot_with_state(const ev_estimator_t * estimator,
                                          const ev_battery_state_t * measured,
                                          ev_estimator_snapshot_t * snapshot);

const char * ev_estimator_state_name(ev_estimator_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* EV_ESTIMATOR_H */
