#include "ev_theme.h"

#include <M5Unified.h>
#include <Preferences.h>

namespace {
struct Palette {
    uint32_t bg;
    uint32_t card;
    uint32_t accent;
    uint32_t warn;
    uint32_t danger;
    uint32_t text;
    uint32_t subtext;
    uint32_t statusbar;
    uint32_t on_accent;
};

/* Night / dark — the original palette (kept identical so nothing regresses). */
const Palette DARK = {
    /* bg        */ 0x101418,
    /* card      */ 0x1b2127,
    /* accent    */ 0x2ecc71,
    /* warn      */ 0xf1c40f,
    /* danger    */ 0xe74c3c,
    /* text      */ 0xf5f6fa,
    /* subtext   */ 0x8a94a0,
    /* statusbar */ 0x0b0f12,
    /* on_accent */ 0x08120b,  /* near-black: reads on the bright dark-theme green */
};

/* Day / light — tuned for sunlight legibility: near-white background, dark
 * high-contrast text, a slightly deeper green accent so it reads on white,
 * warn/danger darkened for contrast against the light background. */
const Palette LIGHT = {
    /* bg        */ 0xeef1f4,
    /* card      */ 0xffffff,
    /* accent    */ 0x188f4d,
    /* warn      */ 0xb8860b,
    /* danger    */ 0xc0392b,
    /* text      */ 0x11161b,
    /* subtext   */ 0x5a6572,
    /* statusbar */ 0xd6dbe0,
    /* on_accent */ 0xffffff,  /* pure white: reads on the deeper light-theme green */
};

ev_theme_mode_t mode = EV_THEME_DARK;
bool light_active = false;  /* which concrete palette is currently applied */

/* NVS persistence. A tiny namespace of our own so it never collides with the
 * logger/location stores. Stored as a single byte (the mode enum). */
constexpr char PREFS_NS[]  = "ev_theme";
constexpr char PREFS_KEY[] = "mode";

void persist_mode()
{
    Preferences prefs;
    if (prefs.begin(PREFS_NS, false /* read-write */)) {
        prefs.putUChar(PREFS_KEY, (uint8_t)mode);
        prefs.end();
    }
}

const Palette & active()
{
    return light_active ? LIGHT : DARK;
}

/* Local timezone offset from UTC, in minutes. The RTC is GPS-synced to UTC, so
 * AUTO day/night must convert to local time first. Default is IST (+5:30). */
constexpr int LOCAL_TZ_OFFSET_MIN = 5 * 60 + 30;  /* Asia/Kolkata, UTC+5:30 */

/* Resolve whether AUTO should be light right now, from the RTC hour converted
 * to local time. Day window is 06:00-17:59 LOCAL. If the clock is unset the RTC
 * year reads ~2000; in that case we keep dark. */
bool auto_wants_light()
{
    auto dt = M5.Rtc.getDateTime();
    if (dt.date.year < 2023) return false;  /* clock unset -> default dark */

    /* UTC minutes-of-day -> local minutes-of-day, wrapped into [0, 1440). */
    int local_min = dt.time.hours * 60 + dt.time.minutes + LOCAL_TZ_OFFSET_MIN;
    local_min = ((local_min % 1440) + 1440) % 1440;
    const int local_hour = local_min / 60;
    return local_hour >= 6 && local_hour < 18;
}

/* Recompute light_active from the mode. Returns true if it changed. */
bool resolve()
{
    const bool prev = light_active;
    switch (mode) {
        case EV_THEME_LIGHT: light_active = true;  break;
        case EV_THEME_DARK:  light_active = false; break;
        case EV_THEME_AUTO:  light_active = auto_wants_light(); break;
        default:             light_active = false; break;
    }
    return light_active != prev;
}
}  // namespace

extern "C" {

lv_color_t ev_theme_bg(void)        { return lv_color_hex(active().bg); }
lv_color_t ev_theme_card(void)      { return lv_color_hex(active().card); }
lv_color_t ev_theme_accent(void)    { return lv_color_hex(active().accent); }
lv_color_t ev_theme_warn(void)      { return lv_color_hex(active().warn); }
lv_color_t ev_theme_danger(void)    { return lv_color_hex(active().danger); }
lv_color_t ev_theme_text(void)      { return lv_color_hex(active().text); }
lv_color_t ev_theme_subtext(void)   { return lv_color_hex(active().subtext); }
lv_color_t ev_theme_statusbar(void) { return lv_color_hex(active().statusbar); }
lv_color_t ev_theme_on_accent(void) { return lv_color_hex(active().on_accent); }

void ev_theme_load(void)
{
    Preferences prefs;
    if (prefs.begin(PREFS_NS, true /* read-only */)) {
        const uint8_t stored = prefs.getUChar(PREFS_KEY, (uint8_t)EV_THEME_DARK);
        prefs.end();
        if (stored < (uint8_t)EV_THEME_MODE_COUNT) {
            mode = (ev_theme_mode_t)stored;
        }
    }
    resolve();
    Serial.printf("Theme: loaded mode %d (%s)\n", (int)mode,
                  light_active ? "light" : "dark");
}

void ev_theme_set_mode(ev_theme_mode_t new_mode)
{
    if (new_mode >= EV_THEME_MODE_COUNT) return;
    mode = new_mode;
    resolve();
    persist_mode();
}

ev_theme_mode_t ev_theme_get_mode(void)
{
    return mode;
}

bool ev_theme_is_light(void)
{
    return light_active;
}

bool ev_theme_refresh(void)
{
    if (mode != EV_THEME_AUTO) return false;
    return resolve();
}

}  // extern "C"
