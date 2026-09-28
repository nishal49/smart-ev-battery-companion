#ifndef EV_LOGGER_H
#define EV_LOGGER_H

#include <stdbool.h>
#include <stdint.h>

#include "model/ev_battery_model.h"
#include "model/ev_estimator.h"

#ifdef __cplusplus
extern "C" {
#endif

/* CSV telemetry logger. Rows are buffered in RAM and flushed to the storage
 * backend periodically, so flash/SD wear stays bounded regardless of sample
 * rate. Storage is chosen at init: SD card if present, else internal LittleFS,
 * so the feature always works even with no card. */

typedef enum {
    EV_LOG_BACKEND_NONE = 0,
    EV_LOG_BACKEND_LITTLEFS,
    EV_LOG_BACKEND_SD
} ev_log_backend_t;

typedef struct {
    ev_log_backend_t backend;
    bool     active;         /* storage mounted and available */
    bool     enabled;        /* user toggle: is logging currently on */
    uint32_t rows_logged;    /* rows accepted since boot */
    uint32_t rows_written;   /* rows actually flushed to storage */
    uint32_t bytes_written;  /* cumulative CSV bytes flushed */
    uint32_t last_flush_ms;
    char     path[48];       /* current log file path */
} ev_logger_status_t;

/* Mount storage and open/append the CSV log. Safe to call once at startup. */
bool ev_logger_init(void);

/* Enable/disable logging at runtime. When disabled, append() is a no-op and no
 * storage is written. Defaults to enabled after init. */
void ev_logger_set_enabled(bool enabled);
bool ev_logger_is_enabled(void);

/* Buffer one telemetry row. Cheap; does not touch storage every call. No-op
 * when logging is disabled. */
void ev_logger_append(const ev_battery_state_t * state,
                      const ev_estimator_snapshot_t * estimator);

/* Flush buffered rows to storage if the interval elapsed or the buffer filled.
 * Call from the main loop; internally rate-limited. */
void ev_logger_service(uint32_t now_ms);

/* Force any buffered rows to storage now (e.g. before shutdown/demo reset). */
void ev_logger_flush(void);

void ev_logger_get_status(ev_logger_status_t * status);
const char * ev_logger_backend_name(ev_log_backend_t backend);

#ifdef __cplusplus
}
#endif

#endif /* EV_LOGGER_H */
