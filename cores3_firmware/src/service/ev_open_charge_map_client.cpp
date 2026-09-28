#include "ev_open_charge_map_client.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ev_app_config.h"
#include "ev_json_allocator.h"

namespace {
const char OCM_ROOT_CA[] PROGMEM = R"PEM(-----BEGIN CERTIFICATE-----
MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD
VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG
A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw
WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz
IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi
AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi
QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR
HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW
BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D
9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8
p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD
-----END CERTIFICATE-----
)PEM";

void copy_text(char * destination, size_t size, const char * source)
{
    if (destination == nullptr || size == 0U) return;
    snprintf(destination, size, "%s", source != nullptr ? source : "");
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
}  // namespace

bool ev_open_charge_map_fetch(double latitude, double longitude,
                              const char * api_key,
                              ev_charge_station_t * stations,
                              uint8_t * station_count,
                              char * error, size_t error_size)
{
    if (stations == nullptr || station_count == nullptr || api_key == nullptr || api_key[0] == '\0') {
        copy_text(error, error_size, "Open Charge Map key missing");
        return false;
    }

    *station_count = 0U;
    char url[256];
    snprintf(url, sizeof(url),
             "https://api.openchargemap.io/v3/poi/?output=json&latitude=%.6f&longitude=%.6f&distance=%u&distanceunit=KM&maxresults=%u&compact=true&verbose=false",
             latitude, longitude, EV_STATION_RADIUS_KM, EV_NAV_MAX_STATIONS);

    WiFiClientSecure secure_client;
    secure_client.setCACert(OCM_ROOT_CA);
    secure_client.setHandshakeTimeout(12U);

    HTTPClient http;
    http.setConnectTimeout(EV_HTTP_TIMEOUT_MS);
    http.setTimeout(EV_HTTP_TIMEOUT_MS);
    http.useHTTP10(true);
    if (!http.begin(secure_client, url)) {
        copy_text(error, error_size, "OCM connection setup failed");
        return false;
    }
    http.addHeader("X-API-Key", api_key);
    http.addHeader("Accept", "application/json");

    const int status = http.GET();
    if (status != HTTP_CODE_OK) {
        snprintf(error, error_size, "OCM HTTP %d", status);
        http.end();
        return false;
    }
    const int response_size = http.getSize();
    if (response_size > 65536) {
        copy_text(error, error_size, "OCM response too large");
        http.end();
        return false;
    }

    EvPsramAllocator allocator;
    JsonDocument filter(&allocator);
    filter[0]["ID"] = true;
    filter[0]["AddressInfo"]["Title"] = true;
    filter[0]["AddressInfo"]["AddressLine1"] = true;
    filter[0]["AddressInfo"]["Town"] = true;
    filter[0]["AddressInfo"]["Latitude"] = true;
    filter[0]["AddressInfo"]["Longitude"] = true;
    filter[0]["AddressInfo"]["Distance"] = true;
    filter[0]["Connections"][0]["ConnectionType"]["Title"] = true;

    JsonDocument document(&allocator);
    const DeserializationError json_error = deserializeJson(
        document, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (json_error) {
        snprintf(error, error_size, "OCM JSON: %s", json_error.c_str());
        return false;
    }

    for (JsonObject item : document.as<JsonArray>()) {
        if (*station_count >= EV_NAV_MAX_STATIONS) break;
        JsonObject address = item["AddressInfo"];
        if (address.isNull() || !address["Latitude"].is<double>() ||
            !address["Longitude"].is<double>()) continue;

        ev_charge_station_t & station = stations[*station_count];
        memset(&station, 0, sizeof(station));
        station.id = item["ID"] | 0U;
        station.latitude = address["Latitude"].as<double>();
        station.longitude = address["Longitude"].as<double>();
        copy_text(station.name, sizeof(station.name), address["Title"] | "Charging station");

        const char * line = address["AddressLine1"] | "";
        const char * town = address["Town"] | "";
        if (line[0] != '\0' && town[0] != '\0') {
            snprintf(station.address, sizeof(station.address), "%s, %s", line, town);
        } else {
            copy_text(station.address, sizeof(station.address), line[0] != '\0' ? line : town);
        }

        JsonArray connections = item["Connections"].as<JsonArray>();
        if (!connections.isNull() && connections.size() > 0U) {
            copy_text(station.connector, sizeof(station.connector),
                      connections[0]["ConnectionType"]["Title"] | "Connector n/a");
        } else {
            copy_text(station.connector, sizeof(station.connector), "Connector n/a");
        }

        station.straight_distance_km = address["Distance"].is<float>()
            ? address["Distance"].as<float>()
            : haversine_km(latitude, longitude, station.latitude, station.longitude);
        station.route_distance_km = -1.0f;
        ++(*station_count);
    }

    if (*station_count == 0U) {
        copy_text(error, error_size, "No stations within search radius");
        return false;
    }
    copy_text(error, error_size, "");
    return true;
}
