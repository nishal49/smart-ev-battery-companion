#include "ev_navigation_runtime.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if __has_include("ev_secrets.h")
#include "ev_secrets.h"
#endif
#include "ev_app_config.h"
#include "platform/ev_station_cache.h"
#include "service/ev_open_charge_map_client.h"
#include "service/ev_route_client.h"

namespace {
SemaphoreHandle_t state_mutex = nullptr;
TaskHandle_t worker_handle = nullptr;
bool runtime_initialized = false;
ev_navigation_snapshot_t state;
bool refresh_requested = false;
bool route_requested = false;
uint8_t requested_station = 0U;
uint32_t fetched_at_unix = 0U;
uint32_t fetched_at_millis = 0U;
uint32_t last_connect_attempt_ms = 0U;
WiFiMulti wifi_multi;
bool wifi_multi_ready = false;

bool has_text(const char * value);  /* defined below */

/* Wi-Fi visibility state for the settings screen. Guarded by state_mutex. */
ev_wifi_status_t wifi_status;
bool wifi_scan_requested = false;

bool ssid_is_known(const char * ssid)
{
    return (has_text(EV_WIFI_SSID)   && strcmp(ssid, EV_WIFI_SSID)   == 0) ||
           (has_text(EV_WIFI_SSID_2) && strcmp(ssid, EV_WIFI_SSID_2) == 0) ||
           (has_text(EV_WIFI_SSID_3) && strcmp(ssid, EV_WIFI_SSID_3) == 0);
}

/* Refresh the connected-network fields from the live WiFi state. */
void update_wifi_link_status()
{
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(20U)) != pdTRUE) return;
    const bool connected = WiFi.status() == WL_CONNECTED;
    bool changed = connected != wifi_status.connected;
    wifi_status.connected = connected;
    if (connected) {
        char ssid[EV_WIFI_SSID_MAX];
        snprintf(ssid, sizeof(ssid), "%s", WiFi.SSID().c_str());
        if (strcmp(ssid, wifi_status.connected_ssid) != 0) changed = true;
        snprintf(wifi_status.connected_ssid, sizeof(wifi_status.connected_ssid), "%s", ssid);
        wifi_status.connected_rssi = (int8_t)WiFi.RSSI();
    } else {
        wifi_status.connected_ssid[0] = '\0';
        wifi_status.connected_rssi = 0;
    }
    if (changed) ++wifi_status.revision;
    xSemaphoreGive(state_mutex);
}

/* Run a blocking scan on the worker thread and publish the results. */
void run_wifi_scan()
{
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(20U)) == pdTRUE) {
        wifi_status.scanning = true;
        ++wifi_status.revision;
        xSemaphoreGive(state_mutex);
    }

    /* show_hidden = true so hotspots that suppress SSID broadcast still count. */
    const int found = WiFi.scanNetworks(false, true);  /* blocking; worker thread only */

    /* Rank all results by RSSI, then keep the strongest EV_WIFI_MAX_SCAN. The
     * ESP32 does not return the scan sorted, so without this a strong network
     * late in the list would be dropped by the cap. */
    int order[64];
    int m = found > 64 ? 64 : (found < 0 ? 0 : found);
    for (int i = 0; i < m; ++i) order[i] = i;
    for (int a = 0; a < m - 1; ++a) {
        for (int b = a + 1; b < m; ++b) {
            if (WiFi.RSSI(order[b]) > WiFi.RSSI(order[a])) {
                const int t = order[a]; order[a] = order[b]; order[b] = t;
            }
        }
    }

    Serial.printf("[WiFiScan] %d network(s) visible (2.4 GHz only):\n", found);
    for (int i = 0; i < m; ++i) {
        const int idx = order[i];
        Serial.printf("  %2d  %4d dBm  %s\n", i, WiFi.RSSI(idx),
                      WiFi.SSID(idx).length() ? WiFi.SSID(idx).c_str() : "<hidden>");
    }

    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(50U)) == pdTRUE) {
        uint8_t n = 0U;
        for (int i = 0; i < m && n < EV_WIFI_MAX_SCAN; ++i) {
            const int idx = order[i];
            String ssid = WiFi.SSID(idx);
            if (ssid.length() == 0) continue;  /* skip hidden in the UI list */
            ev_wifi_scan_entry_t * e = &wifi_status.entries[n];
            snprintf(e->ssid, sizeof(e->ssid), "%s", ssid.c_str());
            e->rssi = (int8_t)WiFi.RSSI(idx);
            e->known = ssid_is_known(e->ssid);
            e->open = WiFi.encryptionType(idx) == WIFI_AUTH_OPEN;
            ++n;
        }
        wifi_status.scan_count = n;
        wifi_status.scanning = false;
        ++wifi_status.revision;
        xSemaphoreGive(state_mutex);
    }
    WiFi.scanDelete();
}

bool has_text(const char * value)
{
    return value != nullptr && value[0] != '\0';
}

/* Register every configured network once. WiFiMulti then picks the strongest
 * available known network on each connect attempt. */
void wifi_multi_setup()
{
    if (wifi_multi_ready) return;
    if (has_text(EV_WIFI_SSID))   wifi_multi.addAP(EV_WIFI_SSID, EV_WIFI_PASSWORD);
    if (has_text(EV_WIFI_SSID_2)) wifi_multi.addAP(EV_WIFI_SSID_2, EV_WIFI_PASSWORD_2);
    if (has_text(EV_WIFI_SSID_3)) wifi_multi.addAP(EV_WIFI_SSID_3, EV_WIFI_PASSWORD_3);
    wifi_multi_ready = true;
}

void copy_message(char * destination, const char * source)
{
    snprintf(destination, EV_NAV_MESSAGE_LEN, "%s", source != nullptr ? source : "");
}

float haversine_km(double lat1, double lon1, double lat2, double lon2)
{
    constexpr double kEarthRadiusKm = 6371.0;
    constexpr double kDegToRad = 0.017453292519943295;
    const double dlat = (lat2 - lat1) * kDegToRad;
    const double dlon = (lon2 - lon1) * kDegToRad;
    const double a = sin(dlat / 2.0) * sin(dlat / 2.0) +
        cos(lat1 * kDegToRad) * cos(lat2 * kDegToRad) *
        sin(dlon / 2.0) * sin(dlon / 2.0);
    return static_cast<float>(kEarthRadiusKm * 2.0 * atan2(sqrt(a), sqrt(1.0 - a)));
}

bool clock_is_valid()
{
    return time(nullptr) >= 1704067200;  // 2024-01-01 UTC
}

bool wait_for_clock()
{
    if (clock_is_valid()) return true;
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    const uint32_t started = millis();
    while (!clock_is_valid() && millis() - started < 8000U) {
        vTaskDelay(pdMS_TO_TICKS(200U));
    }
    return clock_is_valid();
}

void set_station_status(ev_station_status_t status, const char * message)
{
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(50U)) != pdTRUE) return;
    state.station_status = status;
    copy_message(state.message, message);
    ++state.revision;
    xSemaphoreGive(state_mutex);
}

void fail_pending_route(const char * message)
{
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(50U)) != pdTRUE) return;
    state.route_status = EV_ROUTE_ERROR;
    copy_message(state.message, message);
    ++state.route_revision;
    ++state.revision;
    xSemaphoreGive(state_mutex);
}

bool connect_wifi()
{
    if (WiFi.status() == WL_CONNECTED) return true;
    const uint32_t now = millis();
    /* Throttle retries, but never delay the very first attempt. */
    if (last_connect_attempt_ms != 0U && now - last_connect_attempt_ms < 15000U) return false;
    last_connect_attempt_ms = now != 0U ? now : 1U;

    set_station_status(EV_STATION_CONNECTING, "Connecting to Wi-Fi");
    wifi_multi_setup();
    const uint32_t started = millis();
    /* WiFiMulti::run scans and connects to the strongest configured network. */
    while (wifi_multi.run() != WL_CONNECTED &&
           millis() - started < EV_WIFI_CONNECT_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(250U));
    }
    if (WiFi.status() != WL_CONNECTED) {
        bool has_cache = false;
        if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(50U)) == pdTRUE) {
            has_cache = state.station_count > 0U;
            xSemaphoreGive(state_mutex);
        }
        set_station_status(has_cache ? EV_STATION_CACHED : EV_STATION_OFFLINE,
                           "Wi-Fi unavailable");
        return false;
    }

    WiFi.setAutoReconnect(true);
    if (!wait_for_clock()) {
        set_station_status(EV_STATION_ERROR, "Network time unavailable");
        return false;
    }
    return true;
}

void update_station_age()
{
    uint32_t age = EV_NAV_AGE_UNKNOWN;
    if (fetched_at_unix > 0U && clock_is_valid()) {
        const time_t now = time(nullptr);
        age = now >= static_cast<time_t>(fetched_at_unix)
            ? static_cast<uint32_t>(now - fetched_at_unix) : 0U;
    } else if (fetched_at_millis > 0U) {
        age = (millis() - fetched_at_millis) / 1000U;
    }

    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(20U)) != pdTRUE) return;
    if (state.station_age_s != age) {
        state.station_age_s = age;
        ++state.revision;
    }
    xSemaphoreGive(state_mutex);
}

bool fetch_stations()
{
    ev_location_snapshot_t location{};
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(50U)) != pdTRUE) return false;
    location = state.location;
    state.station_status = EV_STATION_FETCHING;
    copy_message(state.message, "Finding nearby chargers");
    ++state.revision;
    xSemaphoreGive(state_mutex);

    ev_charge_station_t stations[EV_NAV_MAX_STATIONS]{};
    uint8_t count = 0U;
    char error[EV_NAV_MESSAGE_LEN]{};
    const bool ok = ev_open_charge_map_fetch(location.latitude, location.longitude,
                                              EV_OCM_API_KEY, stations, &count,
                                              error, sizeof(error));
    if (!ok) {
        bool has_cache = false;
        if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(50U)) == pdTRUE) {
            has_cache = state.station_count > 0U;
            state.station_status = has_cache ? EV_STATION_CACHED : EV_STATION_ERROR;
            copy_message(state.message, error);
            ++state.revision;
            xSemaphoreGive(state_mutex);
        }
        return false;
    }

    const time_t now = time(nullptr);
    fetched_at_unix = clock_is_valid() ? static_cast<uint32_t>(now) : 0U;
    fetched_at_millis = millis();
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(50U)) == pdTRUE) {
        memcpy(state.stations, stations, sizeof(stations));
        state.station_count = count;
        state.station_status = EV_STATION_ONLINE;
        state.station_age_s = 0U;
        state.selected_station = -1;
        state.route_status = EV_ROUTE_IDLE;
        state.route_distance_km = 0.0f;
        state.route_duration_s = 0U;
        state.maneuver_count = 0U;
        copy_message(state.message, "Live Open Charge Map data");
        ++state.station_revision;
        ++state.route_revision;
        ++state.revision;
        xSemaphoreGive(state_mutex);
    }
    ev_station_cache_save(stations, count, fetched_at_unix);
    return true;
}

void fetch_route(uint8_t station_index)
{
    ev_location_snapshot_t location{};
    ev_charge_station_t destination{};
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(50U)) != pdTRUE) return;
    if (station_index >= state.station_count) {
        xSemaphoreGive(state_mutex);
        fail_pending_route("Station selection expired");
        return;
    }
    location = state.location;
    destination = state.stations[station_index];
    state.selected_station = static_cast<int8_t>(station_index);
    state.route_status = EV_ROUTE_FETCHING;
    copy_message(state.message, "Calculating driving route");
    ++state.route_revision;
    ++state.revision;
    xSemaphoreGive(state_mutex);

    float distance_km = 0.0f;
    uint32_t duration_s = 0U;
    static ev_route_maneuver_t maneuvers[EV_NAV_MAX_MANEUVERS];
    static ev_route_point_t route_points[EV_NAV_MAX_ROUTE_POINTS];
    uint8_t maneuver_count = 0U;
    uint8_t route_point_count = 0U;
    char error[EV_NAV_MESSAGE_LEN]{};
    memset(maneuvers, 0, sizeof(maneuvers));
    memset(route_points, 0, sizeof(route_points));
    const bool ok = ev_route_fetch(location.latitude, location.longitude,
                                   destination.latitude, destination.longitude,
                                   EV_ORS_API_KEY, &distance_km, &duration_s,
                                   maneuvers, &maneuver_count,
                                   route_points, &route_point_count,
                                   error, sizeof(error));

    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(50U)) != pdTRUE) return;
    if (ok) {
        state.route_status = EV_ROUTE_READY;
        state.route_distance_km = distance_km;
        state.route_duration_s = duration_s;
        state.maneuver_count = maneuver_count;
        state.current_maneuver = 0U;
        state.route_point_count = route_point_count;
        memcpy(state.maneuvers, maneuvers, sizeof(maneuvers));
        memcpy(state.route_points, route_points, sizeof(route_points));
        if (station_index < state.station_count) {
            state.stations[station_index].route_distance_km = distance_km;
            ++state.station_revision;
        }
        copy_message(state.message, "Driving route ready");
    } else {
        state.route_status = EV_ROUTE_ERROR;
        state.route_distance_km = 0.0f;
        state.route_duration_s = 0U;
        state.maneuver_count = 0U;
        state.route_point_count = 0U;
        copy_message(state.message, error);
    }
    ++state.route_revision;
    ++state.revision;
    xSemaphoreGive(state_mutex);
}

void navigation_worker(void *)
{
    WiFi.mode(WIFI_STA);
    const bool station_configured = has_text(EV_WIFI_SSID) && has_text(EV_OCM_API_KEY);
    const bool route_configured = has_text(EV_WIFI_SSID) && has_text(EV_ORS_API_KEY);
    uint32_t last_age_update_ms = 0U;
    uint32_t next_fetch_due_ms = 0U;
    bool fetch_attempted = false;

    uint32_t last_link_update_ms = 0U;
    for (;;) {
        bool do_refresh = false;
        bool do_route = false;
        bool do_scan = false;
        uint8_t station_index = 0U;
        if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(50U)) == pdTRUE) {
            do_refresh = refresh_requested;
            refresh_requested = false;
            do_route = route_requested;
            station_index = requested_station;
            route_requested = false;
            do_scan = wifi_scan_requested;
            wifi_scan_requested = false;
            xSemaphoreGive(state_mutex);
        }

        /* A scan is available whether or not API credentials exist, so the
           settings screen can show nearby networks before setup. */
        if (do_scan) run_wifi_scan();

        /* Keep the connected-network readout fresh once per second. */
        if (millis() - last_link_update_ms >= 1000U) {
            last_link_update_ms = millis();
            update_wifi_link_status();
        }

        if (!station_configured) {
            if (do_route) fail_pending_route("Add Wi-Fi and API keys");
            vTaskDelay(pdMS_TO_TICKS(250U));
            continue;
        }

        const bool periodic_due = !fetch_attempted ||
            (int32_t)(millis() - next_fetch_due_ms) >= 0;
        if ((do_refresh || periodic_due || do_route) && connect_wifi()) {
            if (do_refresh || periodic_due) {
                /* Retry sooner after a failure so a transient network or API
                   error does not stall discovery for the full refresh period. */
                const bool fetched = fetch_stations();
                fetch_attempted = true;
                next_fetch_due_ms = millis() + (fetched ? EV_STATION_REFRESH_MS : 30000U);
            }
            if (do_route) {
                if (route_configured) fetch_route(station_index);
                else fail_pending_route("openrouteservice key missing");
            }
        } else if (do_route) {
            fail_pending_route("Route needs Wi-Fi");
        }

        if (millis() - last_age_update_ms >= 1000U) {
            last_age_update_ms = millis();
            update_station_age();
        }
        vTaskDelay(pdMS_TO_TICKS(250U));
    }
}
}  // namespace

extern "C" bool ev_navigation_runtime_init(void)
{
    if (runtime_initialized) return true;
    state_mutex = xSemaphoreCreateMutex();
    if (state_mutex == nullptr) return false;

    ev_navigation_state_init(&state);
    state.location.source = EV_LOCATION_CONFIGURED;
    state.location.has_position = true;
    state.location.latitude = EV_CONFIGURED_LATITUDE;
    state.location.longitude = EV_CONFIGURED_LONGITUDE;

    uint8_t cached_count = 0U;
    if (ev_station_cache_load(state.stations, &cached_count, &fetched_at_unix)) {
        state.station_count = cached_count;
        state.station_status = EV_STATION_CACHED;
        copy_message(state.message, "Saved charging stations");
        ++state.station_revision;
    } else if (!has_text(EV_WIFI_SSID) || !has_text(EV_OCM_API_KEY)) {
        state.station_status = EV_STATION_CONFIG_REQUIRED;
        copy_message(state.message, "Add Wi-Fi and API keys");
    } else {
        state.station_status = EV_STATION_OFFLINE;
        copy_message(state.message, "Waiting for network");
    }
    const bool station_configured = has_text(EV_WIFI_SSID) && has_text(EV_OCM_API_KEY);
    refresh_requested = station_configured;
    ++state.revision;
    runtime_initialized = true;

    /* Keep the credential-free indoor demo lightweight: GPS/location snapshots
       remain active, but Wi-Fi/TLS and the worker stack are not allocated. */
    if (!station_configured) {
        Serial.printf("Navigation: credentials required, cache=%u station(s)\n", cached_count);
        return true;
    }

    const BaseType_t result = xTaskCreatePinnedToCore(
        navigation_worker, "ev_navigation", 16384U, nullptr, 1U,
        &worker_handle, 0U);
    if (result != pdPASS) {
        worker_handle = nullptr;
        runtime_initialized = false;
        vSemaphoreDelete(state_mutex);
        state_mutex = nullptr;
        return false;
    }
    Serial.printf("Navigation: %s, cache=%u station(s)\n",
                  has_text(EV_WIFI_SSID) && has_text(EV_OCM_API_KEY)
                      ? "network configured" : "credentials required",
                  cached_count);
    return true;
}

extern "C" void ev_navigation_runtime_set_location(const ev_location_snapshot_t * location)
{
    if (location == nullptr || state_mutex == nullptr) return;
    if (xSemaphoreTake(state_mutex, 0U) != pdTRUE) return;

    /* ~55 m of movement is worth re-scoring distances; only a source change or
       a ~1 km move justifies spending another Open Charge Map query. */
    const float moved_km = haversine_km(state.location.latitude, state.location.longitude,
                                        location->latitude, location->longitude);
    const bool source_change = state.location.source != location->source;
    const bool material_change = source_change || moved_km > 0.05f;
    const bool refetch_change = source_change || moved_km > 1.0f;
    const bool display_change = material_change ||
        state.location.satellites != location->satellites ||
        state.location.fix_age_ms / 1000U != location->fix_age_ms / 1000U ||
        fabsf(state.location.hdop - location->hdop) > 0.1f;
    state.location = *location;

    if (material_change) {
        for (uint8_t i = 0U; i < state.station_count; ++i) {
            state.stations[i].straight_distance_km = haversine_km(
                location->latitude, location->longitude,
                state.stations[i].latitude, state.stations[i].longitude);
        }
        ++state.station_revision;
    }
    if (refetch_change) refresh_requested = true;
    if (display_change) ++state.revision;
    xSemaphoreGive(state_mutex);
}

extern "C" bool ev_navigation_runtime_get_snapshot(ev_navigation_snapshot_t * snapshot)
{
    if (snapshot == nullptr || state_mutex == nullptr ||
        xSemaphoreTake(state_mutex, 0U) != pdTRUE) return false;
    *snapshot = state;
    xSemaphoreGive(state_mutex);
    return true;
}

extern "C" void ev_navigation_runtime_request_wifi_scan(void)
{
    if (state_mutex == nullptr || xSemaphoreTake(state_mutex, 0U) != pdTRUE) return;
    wifi_scan_requested = true;
    xSemaphoreGive(state_mutex);
}

extern "C" bool ev_navigation_runtime_get_wifi_status(ev_wifi_status_t * status)
{
    if (status == nullptr || state_mutex == nullptr ||
        xSemaphoreTake(state_mutex, 0U) != pdTRUE) return false;
    *status = wifi_status;
    xSemaphoreGive(state_mutex);
    return true;
}

extern "C" void ev_navigation_runtime_request_refresh(void)
{
    if (state_mutex == nullptr || xSemaphoreTake(state_mutex, 0U) != pdTRUE) return;
    refresh_requested = true;
    xSemaphoreGive(state_mutex);
}

extern "C" void ev_navigation_runtime_request_route(uint8_t station_index)
{
    if (state_mutex == nullptr || xSemaphoreTake(state_mutex, 0U) != pdTRUE) return;
    if (station_index < state.station_count) {
        requested_station = station_index;
        route_requested = true;
        state.selected_station = static_cast<int8_t>(station_index);
        state.route_status = EV_ROUTE_REQUESTED;
        /* Clear the previous route immediately so the UI never shows the old
           map/steps while the new route is being calculated. */
        state.route_distance_km = 0.0f;
        state.route_duration_s = 0U;
        state.maneuver_count = 0U;
        state.current_maneuver = 0U;
        state.route_point_count = 0U;
        copy_message(state.message, "Route request queued");
        ++state.route_revision;
        ++state.revision;
    }
    xSemaphoreGive(state_mutex);
}
