#ifndef EV_OPEN_CHARGE_MAP_CLIENT_H
#define EV_OPEN_CHARGE_MAP_CLIENT_H

#include <stddef.h>

#include "model/ev_navigation_state.h"

bool ev_open_charge_map_fetch(double latitude, double longitude,
                              const char * api_key,
                              ev_charge_station_t * stations,
                              uint8_t * station_count,
                              char * error, size_t error_size);

#endif /* EV_OPEN_CHARGE_MAP_CLIENT_H */
