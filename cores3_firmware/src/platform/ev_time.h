#ifndef EV_TIME_H
#define EV_TIME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Wall-clock time backed by the CoreS3-SE's BM8563 RTC (via M5Unified M5.Rtc).
 *
 * The RTC keeps time across light-sleep and, on the CoreS3, is battery/backing-
 * cap held for short unpowered gaps. It is authoritative once set. We set it
 * from GPS UTC when a fix arrives (GPS is a free, accurate time source), and
 * otherwise report that the clock is unset so timestamps stay honest.
 */

/* Initialise the RTC helper. Reads whatever the RTC currently holds. Safe to
 * call once during boot after M5.begin(). */
void ev_time_init(void);

/* Set the RTC from a UTC calendar time (e.g. decoded from GPS). Marks the clock
 * as "valid" so ev_time_is_valid() returns true afterwards. Returns false if
 * the values are out of range. Month is 1..12, day 1..31, hour 0..23. */
bool ev_time_set_utc(uint16_t year, uint8_t month, uint8_t day,
                     uint8_t hour, uint8_t minute, uint8_t second);

/* True once the clock has been set from a trusted source this power session
 * (or was already running from a prior set). */
bool ev_time_is_valid(void);

/* Write an ISO-8601 UTC timestamp ("YYYY-MM-DDTHH:MM:SSZ") into out.
 * If the clock is not valid, writes "unset" instead. out_size should be >= 21.
 * Always NUL-terminates. */
void ev_time_now_iso(char * out, uint32_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* EV_TIME_H */
