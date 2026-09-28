#ifndef EV_LIVE_SOURCE_H
#define EV_LIVE_SOURCE_H

#include <stdbool.h>
#include <stdint.h>

#include "ev_battery_model.h"
#include "platform/ev_sensors.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Series cell count of the live bench pack (3S). Exposed so the estimator can
 * be configured with the correct pack geometry when the UI switches to live. */
#define EV_LIVE_PACK_CELLS_S 3U

/* Turns a raw bench sensor reading into the shared ev_battery_state_t, so the
 * dashboard, estimator, alerts and logger consume live data through exactly the
 * same struct they use for the simulation. Values are the REAL measured 3S
 * bench pack, labelled EV_DATA_SOURCE_LIVE_BENCH -- not scaled up to 48 V.
 *
 * SoC is derived from the measured per-cell voltage via the CALCE OCV table
 * (the same curve the estimator inverts), which is honest at rest; under load
 * the EKF corrects it. Coulomb integration refines SoC across successive reads.
 */

typedef struct {
    bool  initialised;
    float soc_pct;          /* running SoC estimate for coulomb integration */
    uint32_t last_ms;
    float capacity_ah;      /* configured bench-pack capacity */
    uint32_t rest_since_ms; /* when |current| last dropped into the rest band */
    bool  resting;          /* true once rest has persisted long enough */
    float session_wh_out;    /* cumulative energy DISCHARGED this session (Wh) */
    float session_wh_in;     /* cumulative energy CHARGED this session (Wh) */
    float peak_power_w;      /* peak |pack power| seen this session (W) */
    uint32_t session_start_ms;  /* first-reading timestamp, for elapsed time */
} ev_live_source_t;

void ev_live_source_init(ev_live_source_t * live, float capacity_ah);

/* Map one sensor reading into *out_state. Returns true if the reading was
 * usable (sensors present and valid); false means the caller should fall back
 * to the simulation. */
bool ev_live_source_update(ev_live_source_t * live,
                           const ev_sensor_reading_t * reading,
                           ev_battery_state_t * out_state);

#ifdef __cplusplus
}
#endif

#endif /* EV_LIVE_SOURCE_H */
