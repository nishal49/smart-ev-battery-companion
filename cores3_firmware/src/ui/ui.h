#ifndef EV_UI_H
#define EV_UI_H

#include "lvgl.h"
#include "model/ev_battery_model.h"
#include "model/ev_estimator.h"
#include "model/ev_navigation_state.h"
#include "platform/ev_navigation_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* M5Stack CoreS3-SE: 320x240 landscape touch display. */
#define EV_DISP_W         320
#define EV_DISP_H         240
#define EV_STATUS_HEIGHT  16
#define EV_NAV_HEIGHT     36
#define EV_CONTENT_H      (EV_DISP_H - EV_STATUS_HEIGHT - EV_NAV_HEIGHT)

/* Colours are resolved at runtime from the active theme (see ev_theme.h) so the
 * UI can switch between night (dark) and day (light). These macros keep the
 * original names/usage; each call returns the current palette's colour. */
#include "ev_theme.h"
#define EV_COLOR_BG        ev_theme_bg()
#define EV_COLOR_CARD      ev_theme_card()
#define EV_COLOR_ACCENT    ev_theme_accent()
#define EV_COLOR_WARN      ev_theme_warn()
#define EV_COLOR_DANGER    ev_theme_danger()
#define EV_COLOR_TEXT      ev_theme_text()
#define EV_COLOR_SUBTEXT   ev_theme_subtext()
#define EV_COLOR_STATUSBAR ev_theme_statusbar()
#define EV_COLOR_ON_ACCENT ev_theme_on_accent()

typedef enum {
    EV_SCREEN_DASHBOARD = 0,
    EV_SCREEN_HEALTH,
    EV_SCREEN_CHARGING,
    EV_SCREEN_TRIPS,
    EV_SCREEN_SETTINGS,
    EV_SCREEN_COUNT
} ev_screen_id_t;

void ev_ui_init(void);
void ev_ui_show_screen(ev_screen_id_t id);

/* Rebuild all screens with the current theme palette (call after switching the
 * theme via ev_theme_set_mode). Preserves model/estimator state. */
void ev_ui_rebuild(void);

/* Boot splash shown over the main UI while the network comes up. */
lv_obj_t * ev_splash_create(void);
void ev_splash_set_progress(int percent, const char * status);
void ev_splash_finish(void);
void ev_ui_set_scenario(ev_scenario_t scenario);
void ev_ui_get_estimator_snapshot(ev_estimator_snapshot_t * snapshot);
bool ev_ui_get_battery_state(ev_battery_state_t * state);
void ev_ui_request_wifi_scan(void);
bool ev_ui_get_wifi_status(ev_wifi_status_t * status);
void ev_ui_zero_current(void);
void ev_ui_set_navigation_snapshot(const ev_navigation_snapshot_t * navigation);
bool ev_ui_take_route_request(uint8_t * station_index);
bool ev_ui_take_refresh_request(void);

lv_obj_t * ev_screen_dashboard_create(lv_obj_t * parent);
lv_obj_t * ev_screen_health_create(lv_obj_t * parent);
lv_obj_t * ev_screen_charging_create(lv_obj_t * parent);
lv_obj_t * ev_screen_trips_create(lv_obj_t * parent);
lv_obj_t * ev_screen_settings_create(lv_obj_t * parent);

void ev_screen_dashboard_update(const ev_battery_state_t * state);
void ev_screen_health_update(const ev_battery_state_t * state);
void ev_screen_charging_update(const ev_battery_state_t * state);
void ev_screen_charging_navigation_update(const ev_navigation_snapshot_t * navigation);
bool ev_screen_charging_take_route_request(uint8_t * station_index);
bool ev_screen_charging_take_refresh_request(void);
void ev_screen_trips_update(const ev_battery_state_t * state);

#ifdef __cplusplus
}
#endif

#endif /* EV_UI_H */
