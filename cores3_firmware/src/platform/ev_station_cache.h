#ifndef EV_STATION_CACHE_H
#define EV_STATION_CACHE_H

#include <stdint.h>

#include "model/ev_navigation_state.h"

bool ev_station_cache_load(ev_charge_station_t * stations,
                           uint8_t * station_count,
                           uint32_t * fetched_at_unix);
bool ev_station_cache_save(const ev_charge_station_t * stations,
                           uint8_t station_count,
                           uint32_t fetched_at_unix);

#endif /* EV_STATION_CACHE_H */
