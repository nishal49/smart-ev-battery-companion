#include "ev_power.h"

#include <Arduino.h>
#include <M5Unified.h>
#include <Preferences.h>

#include "ev_logger.h"

namespace {
/* Idle state machine. Times are relative to the last detected touch. */
enum PowerState { STATE_ACTIVE, STATE_DIMMED, STATE_ASLEEP };

ev_power_config_t cfg = {
    /* saver_enabled   */ false,    /* OFF by default; persisted choice loaded at init */
    /* dim_after_ms    */ 30000U,   /* dim after 30 s idle */
    /* sleep_after_ms  */ 60000U,   /* light-sleep after 60 s idle */
    /* active_brightness */ 128U,
    /* dim_brightness    */ 24U,
};

/* Persist the saver on/off choice so it survives reboots (namespace of our own
 * so it never collides with the theme/logger/location stores). */
constexpr char PREFS_NS[]  = "ev_power";
constexpr char PREFS_KEY[] = "saver";

void load_saver_pref()
{
    Preferences prefs;
    if (prefs.begin(PREFS_NS, true /* read-only */)) {
        cfg.saver_enabled = prefs.getBool(PREFS_KEY, false /* default OFF */);
        prefs.end();
    }
}

void save_saver_pref()
{
    Preferences prefs;
    if (prefs.begin(PREFS_NS, false /* read-write */)) {
        prefs.putBool(PREFS_KEY, cfg.saver_enabled);
        prefs.end();
    }
}

PowerState state = STATE_ACTIVE;
uint32_t last_touch_ms = 0U;
bool was_touched_last = false;

/* The side POWER button: a long hold means "shut down". M5Unified debounces it
 * and exposes wasHold(); we also flush before powering off. */
constexpr uint32_t POWER_HOLD_MS = 800U;

void apply_brightness(uint8_t b)
{
    M5.Display.setBrightness(b);
}

bool touch_active_now()
{
    /* M5.update() is already called each loop by the UI touch reader, but call
     * it here too so power servicing is self-contained and order-independent. */
    return M5.Touch.getCount() > 0;
}

void enter_active()
{
    if (state != STATE_ACTIVE) {
        apply_brightness(cfg.active_brightness);
        state = STATE_ACTIVE;
    }
}

void enter_dimmed()
{
    if (state == STATE_ACTIVE) {
        apply_brightness(cfg.dim_brightness);
        state = STATE_DIMMED;
    }
}

void enter_sleep()
{
    /* Flush any buffered telemetry so a sleep never loses rows, then light-
     * sleep. Light-sleep retains RAM and RTC, so we resume exactly here. We
     * wake on a short timer and re-check touch; the display is off while
     * asleep to cut the largest load. */
    ev_logger_flush();
    apply_brightness(0);
    M5.Display.sleep();
    state = STATE_ASLEEP;
    Serial.println("Power: entering light-sleep (idle). Touch to wake.");
}

void wake_from_sleep()
{
    M5.Display.wakeup();
    apply_brightness(cfg.active_brightness);
    state = STATE_ACTIVE;
    last_touch_ms = millis();
    Serial.println("Power: woke from light-sleep.");
}
}  // namespace

extern "C" void ev_power_init(uint8_t active_brightness)
{
    load_saver_pref();  /* restore the user's saved on/off choice (default OFF) */
    cfg.active_brightness = active_brightness;
    last_touch_ms = millis();
    state = STATE_ACTIVE;
    apply_brightness(active_brightness);
    Serial.printf("Power: saver %s, dim @%lus, sleep @%lus\n",
                  cfg.saver_enabled ? "on" : "off",
                  (unsigned long)(cfg.dim_after_ms / 1000U),
                  (unsigned long)(cfg.sleep_after_ms / 1000U));
}

extern "C" void ev_power_service(void)
{
    M5.update();

    /* --- Graceful shutdown on a long POWER-button hold --- */
    if (M5.BtnPWR.wasHold() || M5.BtnPWR.pressedFor(POWER_HOLD_MS)) {
        ev_power_shutdown();
        return;  /* unreachable if PMIC powers off */
    }

    const uint32_t now = millis();

    /* --- Touch activity resets the idle timer and wakes the device --- */
    const bool touched = touch_active_now();
    if (touched) {
        if (state == STATE_ASLEEP) {
            wake_from_sleep();
        } else {
            enter_active();
        }
        last_touch_ms = now;
    }
    was_touched_last = touched;

    if (!cfg.saver_enabled) {
        /* Saver off: make sure we are always active/bright. */
        if (state != STATE_ACTIVE) enter_active();
        return;
    }

    /* --- Idle progression: active -> dimmed -> asleep --- */
    const uint32_t idle = now - last_touch_ms;

    if (state == STATE_ASLEEP) {
        /* Light-sleep for a short window, then wake to poll touch. If still
         * idle we sleep again; this keeps the device responsive to a tap while
         * spending most of the time asleep. */
        M5.Power.lightSleep(500000U /* us = 0.5 s */);
        if (touch_active_now()) {
            wake_from_sleep();
        }
        return;
    }

    if (idle >= cfg.sleep_after_ms) {
        enter_sleep();
    } else if (idle >= cfg.dim_after_ms) {
        enter_dimmed();
    }
}

extern "C" void ev_power_set_saver_enabled(bool enabled)
{
    cfg.saver_enabled = enabled;
    save_saver_pref();  /* remember across reboots */
    if (!enabled) enter_active();
    last_touch_ms = millis();
    Serial.printf("Power: saver %s\n", enabled ? "enabled" : "disabled");
}

extern "C" bool ev_power_saver_enabled(void)
{
    return cfg.saver_enabled;
}

extern "C" void ev_power_set_active_brightness(uint8_t brightness)
{
    cfg.active_brightness = brightness;
    if (state == STATE_ACTIVE) apply_brightness(brightness);
}

extern "C" void ev_power_shutdown(void)
{
    Serial.println("Power: graceful shutdown requested (flushing logs)...");
    ev_logger_flush();
    delay(50);
    M5.Power.powerOff();
    /* If powerOff() does not cut power (e.g. running from USB on some rigs),
     * fall back to deep sleep so the device at least stops drawing/animating. */
    M5.Display.sleep();
    M5.Power.deepSleep();
}
