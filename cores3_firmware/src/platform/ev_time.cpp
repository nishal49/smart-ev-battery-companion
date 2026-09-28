#include "ev_time.h"

#include <Arduino.h>
#include <M5Unified.h>

namespace {
/* "Valid" means the RTC was set from a trusted source (GPS) this session, or
 * it was already ticking from a previous set (year looks sane). We never invent
 * a time: an unset clock is reported as "unset" so log timestamps stay honest. */
bool clock_valid = false;

bool year_looks_set(int year)
{
    /* BM8563 powers up around 2000; anything >= 2023 means someone set it. */
    return year >= 2023 && year <= 2099;
}
}  // namespace

extern "C" void ev_time_init(void)
{
    /* M5.begin() already brings up the RTC on the CoreS3. Read it and decide
     * whether it already holds a real (previously-set) time. */
    auto dt = M5.Rtc.getDateTime();
    if (year_looks_set(dt.date.year)) {
        clock_valid = true;
        Serial.printf("RTC: already running %04d-%02d-%02d %02d:%02d:%02dZ\n",
                      dt.date.year, dt.date.month, dt.date.date,
                      dt.time.hours, dt.time.minutes, dt.time.seconds);
    } else {
        clock_valid = false;
        Serial.println("RTC: unset (will sync from GPS UTC when a fix arrives)");
    }
}

extern "C" bool ev_time_set_utc(uint16_t year, uint8_t month, uint8_t day,
                                uint8_t hour, uint8_t minute, uint8_t second)
{
    if (year < 2023 || year > 2099) return false;
    if (month < 1 || month > 12) return false;
    if (day < 1 || day > 31) return false;
    if (hour > 23 || minute > 59 || second > 59) return false;

    m5::rtc_datetime_t dt;
    dt.date.year = (int)year;
    dt.date.month = (int)month;
    dt.date.date = (int)day;
    dt.time.hours = (int)hour;
    dt.time.minutes = (int)minute;
    dt.time.seconds = (int)second;
    M5.Rtc.setDateTime(dt);

    if (!clock_valid) {
        Serial.printf("RTC: set from GPS UTC %04u-%02u-%02u %02u:%02u:%02uZ\n",
                      (unsigned)year, (unsigned)month, (unsigned)day,
                      (unsigned)hour, (unsigned)minute, (unsigned)second);
    }
    clock_valid = true;
    return true;
}

extern "C" bool ev_time_is_valid(void)
{
    return clock_valid;
}

extern "C" void ev_time_now_iso(char * out, uint32_t out_size)
{
    if (out == NULL || out_size == 0U) return;
    if (!clock_valid) {
        snprintf(out, out_size, "unset");
        return;
    }
    auto dt = M5.Rtc.getDateTime();
    snprintf(out, out_size, "%04d-%02d-%02dT%02d:%02d:%02dZ",
             dt.date.year, dt.date.month, dt.date.date,
             dt.time.hours, dt.time.minutes, dt.time.seconds);
}
