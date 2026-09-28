#ifndef EV_NAVIGATION_STATE_H
#define EV_NAVIGATION_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EV_NAV_MAX_STATIONS       5U
#define EV_NAV_MAX_MANEUVERS      8U
#define EV_NAV_STATION_NAME_LEN   40U
#define EV_NAV_ADDRESS_LEN        48U
#define EV_NAV_CONNECTOR_LEN      24U
#define EV_NAV_INSTRUCTION_LEN    64U
#define EV_NAV_ROAD_NAME_LEN      28U
#define EV_NAV_MESSAGE_LEN        64U
#define EV_NAV_MAX_ROUTE_POINTS   64U
#define EV_NAV_AGE_UNKNOWN        UINT32_MAX

typedef enum {
    EV_LOCATION_CONFIGURED = 0,  /* factory default, no fix ever obtained */
    EV_LOCATION_SAVED_FIX,       /* last good fix restored from flash (NVS) */
    EV_LOCATION_LAST_FIX,        /* recent fix, temporarily stale */
    EV_LOCATION_GPS_FIX          /* fresh live fix */
} ev_location_source_t;

typedef enum {
    EV_STATION_CONFIG_REQUIRED = 0,
    EV_STATION_CONNECTING,
    EV_STATION_FETCHING,
    EV_STATION_ONLINE,
    EV_STATION_CACHED,
    EV_STATION_OFFLINE,
    EV_STATION_ERROR
} ev_station_status_t;

typedef enum {
    EV_ROUTE_IDLE = 0,
    EV_ROUTE_REQUESTED,
    EV_ROUTE_FETCHING,
    EV_ROUTE_READY,
    EV_ROUTE_ERROR
} ev_route_status_t;

/* How the GPS receiver itself is doing, independent of which coordinates the
 * app ends up using. Lets the UI tell "acquiring, go to open sky" apart from
 * "module never responded" (a wiring fault). */
typedef enum {
    EV_GPS_LINK_NO_DATA = 0,   /* no NMEA bytes seen: check wiring/power */
    EV_GPS_LINK_ACQUIRING,     /* receiving data but no valid fix yet */
    EV_GPS_LINK_FIX            /* valid position fix */
} ev_gps_link_t;

typedef struct {
    ev_location_source_t source;
    bool has_position;
    double latitude;
    double longitude;
    uint32_t fix_age_ms;
    uint8_t satellites;
    float hdop;
    ev_gps_link_t link;        /* receiver link/fix health */
    uint32_t chars_seen;       /* total NMEA chars, 0 => nothing wired/powered */
} ev_location_snapshot_t;

typedef struct {
    uint32_t id;
    char name[EV_NAV_STATION_NAME_LEN];
    char address[EV_NAV_ADDRESS_LEN];
    char connector[EV_NAV_CONNECTOR_LEN];
    double latitude;
    double longitude;
    float straight_distance_km;
    float route_distance_km;
} ev_charge_station_t;

/* Decoded route shape, sampled to a fixed budget for on-screen drawing. */
typedef struct {
    float latitude;
    float longitude;
} ev_route_point_t;

typedef struct {
    char instruction[EV_NAV_INSTRUCTION_LEN];
    char road_name[EV_NAV_ROAD_NAME_LEN];
    float distance_km;
    uint16_t duration_s;
    uint8_t type;
} ev_route_maneuver_t;

typedef struct {
    uint32_t revision;
    uint32_t station_revision;
    uint32_t route_revision;
    ev_location_snapshot_t location;
    ev_station_status_t station_status;
    uint32_t station_age_s;
    uint8_t station_count;
    ev_charge_station_t stations[EV_NAV_MAX_STATIONS];
    int8_t selected_station;
    ev_route_status_t route_status;
    float route_distance_km;
    uint32_t route_duration_s;
    uint8_t maneuver_count;
    uint8_t current_maneuver;
    ev_route_maneuver_t maneuvers[EV_NAV_MAX_MANEUVERS];
    uint8_t route_point_count;
    ev_route_point_t route_points[EV_NAV_MAX_ROUTE_POINTS];
    char message[EV_NAV_MESSAGE_LEN];
} ev_navigation_snapshot_t;

void ev_navigation_state_init(ev_navigation_snapshot_t * state);
const char * ev_location_source_name(ev_location_source_t source);
const char * ev_station_status_name(ev_station_status_t status);
const char * ev_route_status_name(ev_route_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* EV_NAVIGATION_STATE_H */
