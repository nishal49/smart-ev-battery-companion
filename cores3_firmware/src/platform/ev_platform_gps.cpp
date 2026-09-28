#include "ev_platform_gps.h"

#include <Arduino.h>
#include <TinyGPSPlus.h>

#include "ev_app_config.h"
#include "ev_location_store.h"
#include "ev_time.h"

namespace {
HardwareSerial gps_uart(EV_GPS_UART_NUMBER);
TinyGPSPlus gps;
ev_location_snapshot_t location{};
bool has_received_fix = false;
double last_latitude = EV_CONFIGURED_LATITUDE;
double last_longitude = EV_CONFIGURED_LONGITUDE;
uint32_t last_fix_ms = 0;

/* Fallback used when there is no live/recent fix. Starts as the factory
 * default, then becomes the last fix restored from flash, so the device does
 * not jump to Bengaluru's centre after a long dropout or a reboot. */
double fallback_latitude = EV_CONFIGURED_LATITUDE;
double fallback_longitude = EV_CONFIGURED_LONGITUDE;
bool has_saved_fix = false;

/* An in-RAM fallback keeps the on-screen position current with no flash cost.
 * Flash is written only every FLASH_INTERVAL_MS, and only after meaningful
 * movement, purely so the position survives a full power cycle (the demo unit
 * is unplugged/replugged on a power bank). A day of bench testing produces at
 * most a handful of flash writes; boots and reads never write. */
constexpr uint32_t FLASH_INTERVAL_MS = 600000U;   /* 10 minutes */
constexpr double SAVE_MIN_MOVE_DEG = 0.0003;      /* ~33 m */
uint32_t last_flash_ms = 0;
double flashed_latitude = 0.0;
double flashed_longitude = 0.0;

uint32_t elapsed_since(uint32_t now, uint32_t then)
{
    return now - then;
}

void maybe_persist_fix(uint32_t now)
{
    /* Decide whether this fix also warrants a (rare) flash backup. */
    const bool moved =
        fabs(last_latitude - flashed_latitude) > SAVE_MIN_MOVE_DEG ||
        fabs(last_longitude - flashed_longitude) > SAVE_MIN_MOVE_DEG;
    const bool flash_due =
        last_flash_ms == 0U || (now - last_flash_ms) >= FLASH_INTERVAL_MS;
    const bool to_flash = moved && flash_due;

    /* Always keep the in-RAM fallback current so the UI never jumps, but only
     * write flash when to_flash is true (throttled backup for power cycles). */
    fallback_latitude = last_latitude;
    fallback_longitude = last_longitude;
    has_saved_fix = true;

    if (to_flash && ev_location_store_save(last_latitude, last_longitude, 0U)) {
        flashed_latitude = last_latitude;
        flashed_longitude = last_longitude;
        last_flash_ms = now;
    }
}

void maybe_sync_clock()
{
    /* GPS carries accurate UTC. Once the clock is not yet set and the receiver
     * gives a valid date+time, push it into the RTC. Only needed until valid;
     * ev_time_set_utc() is cheap but we skip once the clock is trusted. */
    if (ev_time_is_valid()) return;
    if (!gps.date.isValid() || !gps.time.isValid()) return;
    if (gps.date.year() < 2023) return;  /* NMEA junk / no real fix yet */

    ev_time_set_utc((uint16_t)gps.date.year(), (uint8_t)gps.date.month(),
                    (uint8_t)gps.date.day(), (uint8_t)gps.time.hour(),
                    (uint8_t)gps.time.minute(), (uint8_t)gps.time.second());
}

void refresh_location()
{
    const uint32_t now = millis();
    if (gps.location.isValid() && gps.location.isUpdated()) {
        last_latitude = gps.location.lat();
        last_longitude = gps.location.lng();
        last_fix_ms = now;
        has_received_fix = true;
        maybe_persist_fix(now);
    }
    maybe_sync_clock();

    const uint32_t age = has_received_fix ? elapsed_since(now, last_fix_ms) : UINT32_MAX;
    location.has_position = true;
    const uint32_t satellite_count = gps.satellites.isValid()
        ? gps.satellites.value() : 0U;
    location.satellites = static_cast<uint8_t>(satellite_count > 255U ? 255U : satellite_count);
    location.hdop = gps.hdop.isValid() ? static_cast<float>(gps.hdop.hdop()) : 0.0f;

    /* Receiver link health, decided from raw NMEA throughput and fix validity.
     * TinyGPSPlus counts every processed char; zero means nothing is arriving,
     * which is a wiring/power problem, not merely a weak sky view. */
    const uint32_t chars = gps.charsProcessed();
    location.chars_seen = chars;
    const bool have_current_fix = gps.location.isValid() && age <= EV_GPS_FRESH_FIX_MS;
    if (have_current_fix) {
        location.link = EV_GPS_LINK_FIX;
    } else if (chars > 10U) {
        location.link = EV_GPS_LINK_ACQUIRING;
    } else {
        location.link = EV_GPS_LINK_NO_DATA;
    }

    if (has_received_fix && age <= EV_GPS_FRESH_FIX_MS) {
        location.source = EV_LOCATION_GPS_FIX;
        location.latitude = last_latitude;
        location.longitude = last_longitude;
        location.fix_age_ms = age;
    } else if (has_received_fix && age <= EV_GPS_LAST_FIX_MS) {
        location.source = EV_LOCATION_LAST_FIX;
        location.latitude = last_latitude;
        location.longitude = last_longitude;
        location.fix_age_ms = age;
    } else {
        /* No live/recent fix: prefer the fix restored from flash over the
         * factory default so the map does not jump to the city centre. */
        location.source = has_saved_fix ? EV_LOCATION_SAVED_FIX : EV_LOCATION_CONFIGURED;
        location.latitude = fallback_latitude;
        location.longitude = fallback_longitude;
        location.fix_age_ms = UINT32_MAX;
    }
}
}  // namespace

extern "C" void ev_platform_gps_init(void)
{
    gps_uart.begin(EV_GPS_BAUD, SERIAL_8N1, EV_GPS_RX_PIN, EV_GPS_TX_PIN);

    /* Restore the last known good fix from flash so the first frames show the
     * user's real area rather than the factory default. */
    double lat, lon;
    uint32_t ts;
    if (ev_location_store_load(&lat, &lon, &ts)) {
        fallback_latitude = lat;
        fallback_longitude = lon;
        flashed_latitude = lat;
        flashed_longitude = lon;
        has_saved_fix = true;
        Serial.printf("GPS: restored saved fix %.5f, %.5f\n", lat, lon);
    }

    refresh_location();
    Serial.printf("GPS UART%d: RX=%d TX=%d @ %lu baud\n",
                  EV_GPS_UART_NUMBER, EV_GPS_RX_PIN, EV_GPS_TX_PIN,
                  static_cast<unsigned long>(EV_GPS_BAUD));
}

extern "C" void ev_platform_gps_poll(void)
{
    while (gps_uart.available() > 0) {
        const char c = static_cast<char>(gps_uart.read());
#if EV_GPS_RAW_DEBUG
        /* Echo raw NMEA to USB serial so we can see what the module sends.
         * Healthy output looks like repeating $GPGGA / $GPRMC lines. Garbage
         * means wrong baud; nothing means wiring/power. */
        Serial.write(c);
#endif
        gps.encode(c);
    }
    refresh_location();

#if EV_GPS_RAW_DEBUG
    static uint32_t last_report_ms = 0;
    const uint32_t now = millis();
    if (now - last_report_ms >= 2000U) {
        last_report_ms = now;
        Serial.printf("\n[GPS] chars=%lu sentences=%lu failed=%lu sats=%u fix=%d\n",
                      (unsigned long)gps.charsProcessed(),
                      (unsigned long)gps.sentencesWithFix(),
                      (unsigned long)gps.failedChecksum(),
                      location.satellites,
                      (int)location.link);
    }
#endif
}

extern "C" void ev_platform_gps_get_location(ev_location_snapshot_t * output)
{
    if (output == nullptr) return;
    *output = location;
}
