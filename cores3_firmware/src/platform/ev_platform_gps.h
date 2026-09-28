#ifndef EV_PLATFORM_GPS_H
#define EV_PLATFORM_GPS_H

#include "model/ev_navigation_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void ev_platform_gps_init(void);
void ev_platform_gps_poll(void);
void ev_platform_gps_get_location(ev_location_snapshot_t * location);

#ifdef __cplusplus
}
#endif

#endif /* EV_PLATFORM_GPS_H */
