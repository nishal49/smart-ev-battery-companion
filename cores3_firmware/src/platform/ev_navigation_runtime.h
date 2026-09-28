#ifndef EV_NAVIGATION_RUNTIME_H
#define EV_NAVIGATION_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "model/ev_navigation_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EV_WIFI_MAX_SCAN     8U
#define EV_WIFI_SSID_MAX     33U

typedef struct {
    char    ssid[EV_WIFI_SSID_MAX];
    int8_t  rssi;          /* dBm */
    bool    known;         /* matches a configured network */
    bool    open;          /* no password */
} ev_wifi_scan_entry_t;

typedef struct {
    bool     connected;
    char     connected_ssid[EV_WIFI_SSID_MAX];
    int8_t   connected_rssi;
    bool     scanning;
    uint8_t  scan_count;
    uint32_t revision;     /* bumps when the scan list or link changes */
    ev_wifi_scan_entry_t entries[EV_WIFI_MAX_SCAN];
} ev_wifi_status_t;

bool ev_navigation_runtime_init(void);
void ev_navigation_runtime_set_location(const ev_location_snapshot_t * location);
bool ev_navigation_runtime_get_snapshot(ev_navigation_snapshot_t * snapshot);
void ev_navigation_runtime_request_refresh(void);
void ev_navigation_runtime_request_route(uint8_t station_index);

/* Wi-Fi visibility for the settings screen. The scan runs on the worker
 * thread; the UI polls the status snapshot on the LVGL thread. */
void ev_navigation_runtime_request_wifi_scan(void);
bool ev_navigation_runtime_get_wifi_status(ev_wifi_status_t * status);

#ifdef __cplusplus
}
#endif

#endif /* EV_NAVIGATION_RUNTIME_H */
