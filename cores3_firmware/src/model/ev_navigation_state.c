#include "ev_navigation_state.h"

#include <string.h>

void ev_navigation_state_init(ev_navigation_snapshot_t * state)
{
    if (state == NULL) return;
    memset(state, 0, sizeof(*state));
    state->location.source = EV_LOCATION_CONFIGURED;
    state->station_status = EV_STATION_CONFIG_REQUIRED;
    state->station_age_s = EV_NAV_AGE_UNKNOWN;
    state->selected_station = -1;
    state->route_status = EV_ROUTE_IDLE;
    for (size_t i = 0; i < EV_NAV_MAX_STATIONS; ++i) {
        state->stations[i].route_distance_km = -1.0f;
    }
}

const char * ev_location_source_name(ev_location_source_t source)
{
    switch (source) {
        case EV_LOCATION_GPS_FIX: return "GPS FIX";
        case EV_LOCATION_LAST_FIX: return "LAST FIX";
        case EV_LOCATION_SAVED_FIX: return "SAVED";
        case EV_LOCATION_CONFIGURED:
        default: return "CONFIGURED";
    }
}

const char * ev_station_status_name(ev_station_status_t status)
{
    switch (status) {
        case EV_STATION_CONNECTING: return "CONNECTING";
        case EV_STATION_FETCHING: return "UPDATING";
        case EV_STATION_ONLINE: return "ONLINE";
        case EV_STATION_CACHED: return "CACHED";
        case EV_STATION_OFFLINE: return "OFFLINE";
        case EV_STATION_ERROR: return "ERROR";
        case EV_STATION_CONFIG_REQUIRED:
        default: return "CONFIGURE";
    }
}

const char * ev_route_status_name(ev_route_status_t status)
{
    switch (status) {
        case EV_ROUTE_REQUESTED: return "QUEUED";
        case EV_ROUTE_FETCHING: return "ROUTING";
        case EV_ROUTE_READY: return "ROUTE READY";
        case EV_ROUTE_ERROR: return "ROUTE ERROR";
        case EV_ROUTE_IDLE:
        default: return "SELECT STATION";
    }
}
