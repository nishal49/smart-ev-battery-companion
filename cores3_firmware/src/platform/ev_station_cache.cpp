#include "ev_station_cache.h"

#include <Preferences.h>
#include <stddef.h>
#include <string.h>

namespace {
constexpr uint32_t CACHE_MAGIC = 0x45564348U;
constexpr uint16_t CACHE_VERSION = 1U;
constexpr char CACHE_NAMESPACE[] = "ev_nav";
constexpr char CACHE_KEY[] = "stations";

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t count;
    uint8_t reserved;
    uint32_t fetched_at_unix;
    ev_charge_station_t stations[EV_NAV_MAX_STATIONS];
    uint32_t checksum;
} station_cache_record_t;

uint32_t checksum(const station_cache_record_t & record)
{
    const uint8_t * bytes = reinterpret_cast<const uint8_t *>(&record);
    const size_t length = offsetof(station_cache_record_t, checksum);
    uint32_t value = 2166136261U;
    for (size_t i = 0; i < length; ++i) {
        value = (value ^ bytes[i]) * 16777619U;
    }
    return value;
}
}  // namespace

bool ev_station_cache_load(ev_charge_station_t * stations,
                           uint8_t * station_count,
                           uint32_t * fetched_at_unix)
{
    if (stations == nullptr || station_count == nullptr || fetched_at_unix == nullptr) return false;

    Preferences preferences;
    if (!preferences.begin(CACHE_NAMESPACE, true)) return false;
    if (preferences.getBytesLength(CACHE_KEY) != sizeof(station_cache_record_t)) {
        preferences.end();
        return false;
    }

    station_cache_record_t record{};
    const size_t read = preferences.getBytes(CACHE_KEY, &record, sizeof(record));
    preferences.end();
    if (read != sizeof(record) || record.magic != CACHE_MAGIC ||
        record.version != CACHE_VERSION || record.count > EV_NAV_MAX_STATIONS ||
        record.checksum != checksum(record)) return false;

    memcpy(stations, record.stations, sizeof(record.stations));
    for (size_t i = 0; i < EV_NAV_MAX_STATIONS; ++i) {
        stations[i].route_distance_km = -1.0f;
    }
    *station_count = record.count;
    *fetched_at_unix = record.fetched_at_unix;
    return record.count > 0U;
}

bool ev_station_cache_save(const ev_charge_station_t * stations,
                           uint8_t station_count,
                           uint32_t fetched_at_unix)
{
    if (stations == nullptr || station_count == 0U || station_count > EV_NAV_MAX_STATIONS) return false;

    station_cache_record_t record{};
    record.magic = CACHE_MAGIC;
    record.version = CACHE_VERSION;
    record.count = station_count;
    record.fetched_at_unix = fetched_at_unix;
    memcpy(record.stations, stations, sizeof(record.stations));
    record.checksum = checksum(record);

    Preferences preferences;
    if (!preferences.begin(CACHE_NAMESPACE, false)) return false;
    const size_t written = preferences.putBytes(CACHE_KEY, &record, sizeof(record));
    preferences.end();
    return written == sizeof(record);
}
