#include "ev_logger.h"

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>
#include <string.h>

#include "model/ev_alert_state.h"
#include "ev_time.h"

/* CoreS3 microSD is on the shared SPI bus with these pins (not the default
 * VSPI). A plain SD.begin() will not work on this board. */
#define EV_SD_SCK   36
#define EV_SD_MISO  35
#define EV_SD_MOSI  37
#define EV_SD_CS     4

/* SD is opt-in: it shares the SPI bus and can stall boot with no card. Enable
 * only after confirming the board reads the card. LittleFS is the default. */
#ifndef EV_LOG_TRY_SD
#define EV_LOG_TRY_SD 0
#endif

namespace {
constexpr uint32_t FLUSH_INTERVAL_MS = 15000U;  /* flush at most ~4x/min */
constexpr size_t   FLUSH_AT_BYTES    = 1024U;   /* or when buffer fills */
constexpr char     LOG_PATH[]        = "/ev_log.csv";
/* Hard cap so logging can never fill storage: when the file passes this size
 * it is rotated (old data dropped, header rewritten). ~1 MB ~= 5 h of data. */
constexpr uint32_t MAX_LOG_BYTES     = 1048576U;
constexpr char     CSV_HEADER[] =
    "utc_time,ms,source,scenario,quality,soc_pct,volt_v,curr_a,temp_c,"
    "range_km,ekf_soc_pct,ekf_state,ekf_conf\n";

#if EV_LOG_TRY_SD
SPIClass sd_spi(HSPI);
#endif
fs::FS * fs_backend = nullptr;
ev_logger_status_t status;
String row_buffer;
bool initialised = false;
/* Default OFF so idle/testing never fills storage; user enables it in Settings
 * (or for a demo trace) via the Data log toggle. */
bool enabled = false;

const char * quality_short(ev_data_quality_t q)
{
    switch (q) {
        case EV_DATA_QUALITY_WARNING: return "WARN";
        case EV_DATA_QUALITY_FAULT:   return "FAULT";
        default:                      return "GOOD";
    }
}

#if EV_LOG_TRY_SD
/* Mount SD on the CoreS3 SPI pins. Returns true on success. */
bool mount_sd()
{
    sd_spi.begin(EV_SD_SCK, EV_SD_MISO, EV_SD_MOSI, EV_SD_CS);
    /* 20 MHz is conservative and reliable on the shared bus. */
    if (!SD.begin(EV_SD_CS, sd_spi, 20000000U)) return false;
    const uint8_t type = SD.cardType();
    return type != CARD_NONE;
}
#endif

void ensure_header(fs::FS & fs)
{
    if (fs.exists(LOG_PATH)) return;
    File f = fs.open(LOG_PATH, FILE_WRITE);
    if (!f) return;
    f.print(CSV_HEADER);
    f.close();
}
}  // namespace

extern "C" bool ev_logger_init(void)
{
    if (initialised) return status.active;
    memset(&status, 0, sizeof(status));
    row_buffer.reserve(FLUSH_AT_BYTES + 256U);

    /* SD sits on the shared SPI bus and can stall boot when no card is present,
     * so it is opt-in (see EV_LOG_TRY_SD). Internal LittleFS always works and
     * is the default; SD becomes a tested upgrade once the card is confirmed. */
#if EV_LOG_TRY_SD
    Serial.println("Logger: trying SD...");
    if (mount_sd()) {
        fs_backend = &SD;
        status.backend = EV_LOG_BACKEND_SD;
        Serial.println("Logger: SD mounted");
    }
#endif

    if (fs_backend == nullptr) {
        Serial.println("Logger: mounting LittleFS (formats once on first boot)...");
        if (LittleFS.begin(true)) {
            fs_backend = &LittleFS;
            status.backend = EV_LOG_BACKEND_LITTLEFS;
            Serial.println("Logger: LittleFS mounted");
        } else {
            status.backend = EV_LOG_BACKEND_NONE;
            status.active = false;
            initialised = true;
            Serial.println("Logger: no storage available");
            return false;
        }
    }

    ensure_header(*fs_backend);
    snprintf(status.path, sizeof(status.path), "%s", LOG_PATH);
    status.active = true;
    status.enabled = enabled;
    initialised = true;
    Serial.printf("Logger: %s active at %s\n",
                  ev_logger_backend_name(status.backend), status.path);
    return true;
}

extern "C" void ev_logger_set_enabled(bool on)
{
    if (enabled == on) return;
    enabled = on;
    status.enabled = on;
    if (!on) ev_logger_flush();  /* persist what we have before pausing */
}

extern "C" bool ev_logger_is_enabled(void)
{
    return enabled;
}

extern "C" void ev_logger_append(const ev_battery_state_t * state,
                                 const ev_estimator_snapshot_t * estimator)
{
    if (!status.active || !enabled || state == NULL) return;

    char line[224];
    char iso_time[24];
    ev_time_now_iso(iso_time, sizeof(iso_time));
    const float ekf_soc = estimator != NULL ? estimator->soc_pct : -1.0f;
    const char * ekf_state = estimator != NULL
        ? ev_estimator_state_name(estimator->state) : "NA";
    const float ekf_conf = estimator != NULL ? estimator->confidence : 0.0f;

    snprintf(line, sizeof(line),
             "%s,%lu,%s,%s,%s,%.1f,%.2f,%.2f,%.1f,%.1f,%.1f,%s,%.2f\n",
             iso_time,
             (unsigned long)millis(),
             ev_data_source_name(state->source),
             ev_scenario_name(state->scenario),
             quality_short(state->quality),
             (double)state->soc_pct,
             (double)state->voltage_v,
             (double)state->current_a,
             (double)state->temperature_c,
             (double)state->conservative_range_km,
             (double)ekf_soc,
             ekf_state,
             (double)ekf_conf);

    row_buffer += line;
    status.rows_logged++;
}

extern "C" void ev_logger_flush(void)
{
    if (!status.active || fs_backend == nullptr || row_buffer.isEmpty()) return;

    /* Rotate before appending if the file has grown past the cap, so logging
     * can never fill storage no matter how long it runs. */
    {
        File probe = fs_backend->open(status.path, FILE_READ);
        const size_t sz = probe ? probe.size() : 0U;
        if (probe) probe.close();
        if (sz >= MAX_LOG_BYTES) {
            fs_backend->remove(status.path);
            ensure_header(*fs_backend);
        }
    }

    File f = fs_backend->open(status.path, FILE_APPEND);
    if (!f) {
        /* Storage vanished (card pulled). Stop cleanly rather than spin. */
        status.active = false;
        Serial.println("Logger: append failed, storage lost");
        return;
    }
    const size_t written = f.print(row_buffer);
    f.close();

    status.bytes_written += (uint32_t)written;
    status.rows_written = status.rows_logged;
    status.last_flush_ms = millis();
    row_buffer = "";
}

extern "C" void ev_logger_service(uint32_t now_ms)
{
    if (!status.active) return;
    const bool due = (now_ms - status.last_flush_ms) >= FLUSH_INTERVAL_MS;
    const bool full = row_buffer.length() >= FLUSH_AT_BYTES;
    if (due || full) ev_logger_flush();
}

extern "C" void ev_logger_get_status(ev_logger_status_t * out)
{
    if (out != NULL) *out = status;
}

extern "C" const char * ev_logger_backend_name(ev_log_backend_t backend)
{
    switch (backend) {
        case EV_LOG_BACKEND_SD:       return "SD card";
        case EV_LOG_BACKEND_LITTLEFS: return "internal flash";
        default:                      return "none";
    }
}
