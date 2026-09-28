#ifndef EV_LOCATION_STORE_H
#define EV_LOCATION_STORE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Persist the last known good GPS fix in NVS (flash) so the device resumes
 * near where it last had a fix instead of the factory default, even across a
 * power cycle. No SD card required. */

bool ev_location_store_load(double * latitude, double * longitude,
                            uint32_t * saved_unix);

/* Save the current fix to NVS flash. The caller throttles calls (after ~33 m
 * of movement, at most once per 10 minutes) so flash wear stays negligible. */
bool ev_location_store_save(double latitude, double longitude,
                            uint32_t saved_unix);

#ifdef __cplusplus
}
#endif

#endif /* EV_LOCATION_STORE_H */
