#include "ev_route_client.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <stdio.h>
#include <string.h>

#include "ev_app_config.h"
#include "ev_json_allocator.h"

namespace {
const char ORS_ROOT_CA[] PROGMEM = R"PEM(-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)PEM";

void copy_text(char * destination, size_t size, const char * source)
{
    if (destination == nullptr || size == 0U) return;
    snprintf(destination, size, "%s", source != nullptr ? source : "");
}

/* Google/openrouteservice encoded polyline, precision 5. Returns false at the
   end of the string. */
bool next_varint(const char * text, size_t length, size_t & index, int32_t & delta)
{
    int32_t result = 0;
    int shift = 0;
    uint8_t chunk = 0;
    if (index >= length) return false;
    do {
        if (index >= length) return false;
        chunk = static_cast<uint8_t>(text[index++]) - 63U;
        result |= static_cast<int32_t>(chunk & 0x1FU) << shift;
        shift += 5;
    } while (chunk >= 0x20U && shift < 32);
    delta = (result & 1) ? ~(result >> 1) : (result >> 1);
    return true;
}

uint32_t count_polyline_points(const char * text, size_t length)
{
    size_t index = 0U;
    uint32_t count = 0U;
    int32_t lat_delta = 0;
    int32_t lon_delta = 0;
    while (next_varint(text, length, index, lat_delta) &&
           next_varint(text, length, index, lon_delta)) {
        ++count;
    }
    return count;
}

/* Decodes with a fixed stride so any route length fits the point budget,
   always keeping the final point so the shape reaches the destination. */
uint8_t decode_polyline_sampled(const char * text, size_t length,
                                ev_route_point_t * points, uint8_t max_points)
{
    const uint32_t total = count_polyline_points(text, length);
    if (total == 0U || max_points == 0U) return 0U;
    const uint32_t stride = (total + max_points - 1U) / max_points;

    size_t index = 0U;
    int32_t lat = 0;
    int32_t lon = 0;
    int32_t lat_delta = 0;
    int32_t lon_delta = 0;
    uint32_t position = 0U;
    uint8_t stored = 0U;

    while (next_varint(text, length, index, lat_delta) &&
           next_varint(text, length, index, lon_delta)) {
        lat += lat_delta;
        lon += lon_delta;
        const bool is_last = (position + 1U) == total;
        if ((position % stride == 0U || is_last) && stored < max_points) {
            points[stored].latitude = static_cast<float>(lat) / 100000.0f;
            points[stored].longitude = static_cast<float>(lon) / 100000.0f;
            ++stored;
        }
        ++position;
    }
    return stored;
}
}  // namespace

bool ev_route_fetch(double from_latitude, double from_longitude,
                    double to_latitude, double to_longitude,
                    const char * api_key,
                    float * distance_km, uint32_t * duration_s,
                    ev_route_maneuver_t * maneuvers,
                    uint8_t * maneuver_count,
                    ev_route_point_t * route_points,
                    uint8_t * route_point_count,
                    char * error, size_t error_size)
{
    if (api_key == nullptr || api_key[0] == '\0' || distance_km == nullptr ||
        duration_s == nullptr || maneuvers == nullptr || maneuver_count == nullptr ||
        route_points == nullptr || route_point_count == nullptr) {
        copy_text(error, error_size, "Routing key missing");
        return false;
    }

    *route_point_count = 0U;
    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"coordinates\":[[%.6f,%.6f],[%.6f,%.6f]],\"instructions\":true,\"geometry\":true,\"units\":\"km\"}",
             from_longitude, from_latitude, to_longitude, to_latitude);

    WiFiClientSecure secure_client;
    secure_client.setCACert(ORS_ROOT_CA);
    secure_client.setHandshakeTimeout(12U);

    HTTPClient http;
    http.setConnectTimeout(EV_HTTP_TIMEOUT_MS);
    http.setTimeout(EV_HTTP_TIMEOUT_MS);
    http.useHTTP10(true);
    if (!http.begin(secure_client,
                    "https://api.openrouteservice.org/v2/directions/driving-car/json")) {
        copy_text(error, error_size, "Route connection setup failed");
        return false;
    }
    http.addHeader("Authorization", api_key);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Accept", "application/json");

    const int status = http.POST(reinterpret_cast<uint8_t *>(payload), strlen(payload));
    if (status != HTTP_CODE_OK) {
        snprintf(error, error_size, "Route HTTP %d", status);
        http.end();
        return false;
    }
    const int response_size = http.getSize();
    if (response_size > 98304) {
        copy_text(error, error_size, "Route response too large");
        http.end();
        return false;
    }

    EvPsramAllocator allocator;
    JsonDocument filter(&allocator);
    filter["routes"][0]["geometry"] = true;
    filter["routes"][0]["summary"]["distance"] = true;
    filter["routes"][0]["summary"]["duration"] = true;
    filter["routes"][0]["segments"][0]["steps"][0]["instruction"] = true;
    filter["routes"][0]["segments"][0]["steps"][0]["name"] = true;
    filter["routes"][0]["segments"][0]["steps"][0]["distance"] = true;
    filter["routes"][0]["segments"][0]["steps"][0]["duration"] = true;
    filter["routes"][0]["segments"][0]["steps"][0]["type"] = true;

    JsonDocument document(&allocator);
    const DeserializationError json_error = deserializeJson(
        document, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (json_error) {
        snprintf(error, error_size, "Route JSON: %s", json_error.c_str());
        return false;
    }

    JsonObject route = document["routes"][0];
    JsonObject summary = route["summary"];
    JsonArray steps = route["segments"][0]["steps"].as<JsonArray>();
    if (route.isNull() || summary.isNull() || steps.isNull()) {
        copy_text(error, error_size, "Route response incomplete");
        return false;
    }

    *distance_km = summary["distance"] | 0.0f;
    *duration_s = static_cast<uint32_t>((summary["duration"] | 0.0f) + 0.5f);
    *maneuver_count = 0U;
    for (JsonObject step : steps) {
        if (*maneuver_count >= EV_NAV_MAX_MANEUVERS) break;
        ev_route_maneuver_t & maneuver = maneuvers[*maneuver_count];
        memset(&maneuver, 0, sizeof(maneuver));
        copy_text(maneuver.instruction, sizeof(maneuver.instruction),
                  step["instruction"] | "Continue");
        copy_text(maneuver.road_name, sizeof(maneuver.road_name), step["name"] | "");
        maneuver.distance_km = step["distance"] | 0.0f;
        const uint32_t step_duration =
            static_cast<uint32_t>((step["duration"] | 0.0f) + 0.5f);
        maneuver.duration_s = static_cast<uint16_t>(
            step_duration > 65535U ? 65535U : step_duration);
        maneuver.type = step["type"] | 0U;
        ++(*maneuver_count);
    }

    if (*maneuver_count == 0U) {
        copy_text(error, error_size, "Route contains no instructions");
        return false;
    }

    JsonString geometry = route["geometry"];
    if (!geometry.isNull() && geometry.size() > 0U) {
        *route_point_count = decode_polyline_sampled(
            geometry.c_str(), geometry.size(), route_points, EV_NAV_MAX_ROUTE_POINTS);
    }

    copy_text(error, error_size, "");
    return true;
}
