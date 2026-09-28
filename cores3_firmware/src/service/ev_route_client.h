#ifndef EV_ROUTE_CLIENT_H
#define EV_ROUTE_CLIENT_H

#include <stddef.h>

#include "model/ev_navigation_state.h"

bool ev_route_fetch(double from_latitude, double from_longitude,
                    double to_latitude, double to_longitude,
                    const char * api_key,
                    float * distance_km, uint32_t * duration_s,
                    ev_route_maneuver_t * maneuvers,
                    uint8_t * maneuver_count,
                    ev_route_point_t * route_points,
                    uint8_t * route_point_count,
                    char * error, size_t error_size);

#endif /* EV_ROUTE_CLIENT_H */
