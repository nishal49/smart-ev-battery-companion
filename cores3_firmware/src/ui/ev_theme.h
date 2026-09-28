#ifndef EV_THEME_H
#define EV_THEME_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Runtime UI theme. The palette is held in variables (not compile-time
 * constants) so the UI can switch between a night (dark) and day (light) look
 * at runtime. `ui.h` maps the EV_COLOR_* names onto these accessors, so every
 * screen reads the active palette without per-widget edits.
 *
 * Switching the theme changes what the accessors return; the caller must then
 * rebuild the screens (ev_ui_rebuild) so already-created widgets pick up the
 * new colours — LVGL does not re-theme existing objects automatically.
 */

typedef enum {
    EV_THEME_DARK = 0,   /* night: dark background, light text (default) */
    EV_THEME_LIGHT,      /* day: light background, dark text, sunlight-legible */
    EV_THEME_AUTO,       /* pick dark/light from the RTC hour (day 06:00-18:00) */
    EV_THEME_MODE_COUNT
} ev_theme_mode_t;

/* Active palette accessors (return by value, usable exactly like the old
 * lv_color_hex(...) macros). */
lv_color_t ev_theme_bg(void);
lv_color_t ev_theme_card(void);
lv_color_t ev_theme_accent(void);
lv_color_t ev_theme_warn(void);
lv_color_t ev_theme_danger(void);
lv_color_t ev_theme_text(void);
lv_color_t ev_theme_subtext(void);

/* The status-bar background (slightly darker/lighter than the card). */
lv_color_t ev_theme_statusbar(void);

/* High-contrast colour to draw ON TOP of the accent fill (e.g. the icon inside
 * the selected nav button). Chosen to stay legible against the accent in both
 * themes, unlike the plain background colour which can wash out. */
lv_color_t ev_theme_on_accent(void);

/* Load the saved theme mode from NVS (call once at boot, before the UI is
 * built, so the first render uses the user's chosen theme). Defaults to DARK if
 * nothing is stored. */
void ev_theme_load(void);

/* Set the theme mode and persist it to NVS. For AUTO, the concrete dark/light
 * choice is resolved from the RTC when this is called (and re-resolved via
 * ev_theme_refresh). */
void ev_theme_set_mode(ev_theme_mode_t mode);
ev_theme_mode_t ev_theme_get_mode(void);

/* True if the currently-applied concrete palette is the light (day) one. */
bool ev_theme_is_light(void);

/* Re-resolve AUTO against the current RTC hour. Returns true if the concrete
 * palette changed (so the caller should rebuild the UI). No-op for DARK/LIGHT. */
bool ev_theme_refresh(void);

#ifdef __cplusplus
}
#endif

#endif /* EV_THEME_H */
