#include <Arduino.h>
#include <M5Unified.h>
#include <lvgl.h>

#include "platform/ev_logger.h"
#include "platform/ev_navigation_runtime.h"
#include "platform/ev_platform_audio.h"
#include "platform/ev_platform_gps.h"
#include "platform/ev_power.h"
#include "platform/ev_time.h"
#include "ui/ev_theme.h"
#include "ui/ui.h"

namespace {

constexpr uint16_t DISPLAY_WIDTH = 320;
constexpr uint16_t DISPLAY_HEIGHT = 240;
constexpr uint16_t DRAW_BUFFER_LINES = 30;
constexpr uint8_t DISPLAY_BRIGHTNESS = 128;
constexpr uint8_t SPEAKER_VOLUME = 64;

alignas(4) uint8_t draw_buffer[DISPLAY_WIDTH * DRAW_BUFFER_LINES * 2];
bool speaker_ready = false;
bool lvgl_ready = false;
bool navigation_started = false;
bool navigation_ready = false;
bool splash_done = false;
bool splash_baseline_captured = false;
uint32_t splash_baseline_revision = 0U;
uint32_t ui_started_ms = 0U;
uint32_t last_log_append_ms = 0U;
uint8_t  boot_step = 0U;
bool     logger_started = false;
bool     auto_zero_done = false;
uint32_t splash_done_ms = 0U;
uint32_t last_navigation_service_ms = 0U;

/* Kept at file scope: these snapshots are far too large to place on the
   Arduino loop task stack alongside LVGL rendering. */
ev_location_snapshot_t gps_location;
ev_navigation_snapshot_t navigation_snapshot;

void service_navigation()
{
    ev_platform_gps_poll();

    if (millis() - last_navigation_service_ms < 250U) return;
    last_navigation_service_ms = millis();

    ev_platform_gps_get_location(&gps_location);
    ev_navigation_runtime_set_location(&gps_location);

    uint8_t station_index = 0U;
    if (ev_ui_take_route_request(&station_index)) {
        ev_navigation_runtime_request_route(station_index);
    }
    if (ev_ui_take_refresh_request()) {
        ev_navigation_runtime_request_refresh();
    }

    if (ev_navigation_runtime_get_snapshot(&navigation_snapshot)) {
        ev_ui_set_navigation_snapshot(&navigation_snapshot);
    }
}

uint32_t lvgl_tick_ms()
{
    return millis();
}

void display_flush(lv_display_t * display, const lv_area_t * area, uint8_t * pixel_map)
{
    const uint32_t width = static_cast<uint32_t>(area->x2 - area->x1 + 1);
    const uint32_t height = static_cast<uint32_t>(area->y2 - area->y1 + 1);

    /* LVGL and M5GFX use opposite RGB565 byte order on this SPI path. */
    lv_draw_sw_rgb565_swap(pixel_map, width * height);
    M5.Display.pushImage(
        area->x1,
        area->y1,
        static_cast<int32_t>(width),
        static_cast<int32_t>(height),
        reinterpret_cast<uint16_t *>(pixel_map)
    );

    lv_display_flush_ready(display);
}

void touch_read(lv_indev_t * input_device, lv_indev_data_t * data)
{
    LV_UNUSED(input_device);
    M5.update();

    if (M5.Touch.getCount() == 0) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    const auto touch = M5.Touch.getDetail(0);
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = touch.x;
    data->point.y = touch.y;
}

bool initialize_lvgl()
{
    lv_init();
    lv_tick_set_cb(lvgl_tick_ms);

    lv_display_t * display = lv_display_create(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    if (display == nullptr) {
        return false;
    }

    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, display_flush);
    lv_display_set_buffers(
        display,
        draw_buffer,
        nullptr,
        sizeof(draw_buffer),
        LV_DISPLAY_RENDER_MODE_PARTIAL
    );

    lv_indev_t * touch = lv_indev_create();
    if (touch == nullptr) {
        return false;
    }

    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, touch_read);
    lv_indev_set_display(touch, display);
    return true;
}

}  // namespace

extern "C" void ev_platform_play_alert_tone(uint16_t frequency_hz, uint32_t duration_ms)
{
    if (!speaker_ready) return;
    M5.Speaker.tone(static_cast<float>(frequency_hz), duration_ms, 0, true);
}

void setup()
{
    Serial.begin(115200);
    delay(500);

    auto config = M5.config();
    M5.begin(config);
    speaker_ready = M5.Speaker.isEnabled() && M5.Speaker.begin();
    if (speaker_ready) {
        M5.Speaker.setVolume(SPEAKER_VOLUME);
    }

    /* BM8563 RTC: read current time; it gets synced from GPS UTC on first fix. */
    ev_time_init();

    /* AXP2101 power management: graceful power-button shutdown + idle saver. */
    ev_power_init(DISPLAY_BRIGHTNESS);

    M5.Display.setBrightness(DISPLAY_BRIGHTNESS);
    M5.Display.fillScreen(TFT_BLACK);

    Serial.printf(
        "CoreS3-SE display: %d x %d\n",
        M5.Display.width(),
        M5.Display.height()
    );
    Serial.printf("CoreS3-SE speaker: %s\n", speaker_ready ? "ready" : "unavailable");

    if (M5.Display.width() != DISPLAY_WIDTH || M5.Display.height() != DISPLAY_HEIGHT) {
        Serial.println("ERROR: Expected a 320x240 landscape display");
        M5.Display.setTextColor(TFT_RED, TFT_BLACK);
        M5.Display.drawString("Display rotation error", 10, 10);
        return;
    }

    if (!initialize_lvgl()) {
        Serial.println("ERROR: LVGL initialization failed");
        M5.Display.setTextColor(TFT_RED, TFT_BLACK);
        M5.Display.drawString("LVGL init failed", 10, 10);
        return;
    }

    /* Restore the saved theme (Dark/Light/Auto) before building the UI so the
     * first render already uses the user's choice. Auto resolves against the
     * RTC, which was initialised just above. */
    ev_theme_load();

    /* Build the main UI on the default screen, then cover it with the splash.
     * ev_splash_finish() fades back to the main UI once boot is done. */
    ev_ui_init();
    ev_splash_create();
    ev_splash_set_progress(15, "Starting up");
    lvgl_ready = true;
    ui_started_ms = millis();
    Serial.println("LVGL EV UI initialized; splash shown, navigation deferred");
}

void loop()
{
    if (!lvgl_ready) {
        delay(100U);
        return;
    }

    /* Bring subsystems up ONE per loop iteration so lv_timer_handler() runs in
     * between and the splash actually repaints/animates. Doing it all in a
     * single iteration froze the screen on the first frame ("Starting up")
     * because no redraw happened until everything finished. */
    if (!navigation_started) {
        const uint32_t since_start = millis() - ui_started_ms;
        switch (boot_step) {
            case 0:
                if (since_start >= 600U) {
                    ev_splash_set_progress(45, "Starting GPS");
                    boot_step = 1;
                }
                break;
            case 1:
                ev_platform_gps_init();
                ev_splash_set_progress(65, "Connecting network");
                boot_step = 2;
                break;
            case 2:
                navigation_ready = ev_navigation_runtime_init();
                Serial.printf("Navigation runtime: %s\n",
                              navigation_ready ? "ready" : "failed");
                navigation_started = true;
                break;
            default:
                break;
        }
    }

    if (navigation_ready) {
        service_navigation();
    }

    /* Initialise logging a short delay AFTER the splash dismissed, so the
     * splash fade-out has fully rendered first. The SD mount blocks the LVGL
     * loop briefly, which would otherwise freeze the fade mid-animation and
     * leave the last splash frame stuck on screen. */
    if (splash_done && !logger_started) {
        if (splash_done_ms == 0U) {
            splash_done_ms = millis();
        } else if (millis() - splash_done_ms >= 500U) {
            logger_started = true;
            ev_logger_init();
        }
    }

    /* Auto-zero the current sensor once, ~2 s after the splash clears, so the
     * demo starts with a clean 0.0 A without touching Settings. The sensor task
     * is publishing stable readings by then, and the ACS723 offset is captured
     * with (the assumption of) NO LOAD flowing -- power the device up with the
     * load OFF, then switch the load on for the demo. Runs exactly once. */
    if (splash_done && !auto_zero_done && splash_done_ms != 0U &&
        millis() - splash_done_ms >= 2000U) {
        ev_ui_zero_current();
        auto_zero_done = true;
        Serial.println("Auto-zeroed current at boot (assumes no load).");
    }

    /* Telemetry logging: buffer one row/second, flush on the logger's own
     * interval. Uses the same battery state + estimator the UI shows. */
    if (logger_started && navigation_started && millis() - last_log_append_ms >= 1000U) {
        last_log_append_ms = millis();
        ev_battery_state_t log_state;
        ev_estimator_snapshot_t log_est;
        if (ev_ui_get_battery_state(&log_state)) {
            ev_ui_get_estimator_snapshot(&log_est);
            ev_logger_append(&log_state, &log_est);
        }
    }
    ev_logger_service(millis());

    /* Dismiss the splash once THIS boot has produced a fresh station result,
     * or after a hard cap. We must not accept the CACHED status that
     * ev_navigation_runtime_init sets from NVS before Wi-Fi even connects, or
     * the splash dismisses instantly on restart while Wi-Fi is still trying.
     * So we require the station_revision to advance past its boot value. */
    if (!splash_done && navigation_started) {
        const uint32_t since_start = millis() - ui_started_ms;
        bool fresh_result = false;
        if (ev_navigation_runtime_get_snapshot(&navigation_snapshot)) {
            if (!splash_baseline_captured) {
                splash_baseline_revision = navigation_snapshot.station_revision;
                splash_baseline_captured = true;
            }
            /* A new fetch (online or, after a failed connect, cached) bumps the
             * revision. That is the real "network settled this boot" signal. */
            const bool advanced =
                navigation_snapshot.station_revision != splash_baseline_revision;
            const bool connected =
                navigation_snapshot.station_status == EV_STATION_ONLINE ||
                navigation_snapshot.station_status == EV_STATION_OFFLINE ||
                navigation_snapshot.station_status == EV_STATION_ERROR;
            fresh_result = advanced || connected;
        }
        if (fresh_result || since_start >= 8000U) {
            splash_done = true;
            const bool online = navigation_snapshot.station_status == EV_STATION_ONLINE ||
                                navigation_snapshot.station_status == EV_STATION_CACHED;
            ev_splash_set_progress(100, online ? "Ready" : "Ready (offline)");
            ev_splash_finish();
        } else {
            /* Ramp the bar between 65 and 95% while waiting on the network. */
            const int pct = 65 + (int)((since_start > 600U ? since_start - 600U : 0U) *
                                       30U / 7400U);
            ev_splash_set_progress(pct < 95 ? pct : 95, "Connecting network");
        }
    }

    /* Power management: only run the idle saver once boot/splash is done, so
     * the device never dims or sleeps while it is still coming up. The power
     * button is polled inside here too; a long hold shuts down cleanly. */
    if (splash_done) {
        ev_power_service();
    }

    uint32_t wait_ms = lv_timer_handler();
    if (wait_ms == LV_NO_TIMER_READY || wait_ms > 20U) {
        wait_ms = 20U;
    } else if (wait_ms < 1U) {
        wait_ms = 1U;
    }

    delay(wait_ms);
}
