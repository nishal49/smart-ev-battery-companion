#include "ui.h"
#include "model/ev_alert_state.h"
#include "model/ev_estimator.h"
#include "model/ev_live_source.h"
#include "platform/ev_navigation_runtime.h"
#include "platform/ev_platform_audio.h"
#include "platform/ev_sensors.h"

#include <stdint.h>

static lv_obj_t * s_content_area;
static lv_obj_t * s_screens[EV_SCREEN_COUNT];
static lv_obj_t * s_nav_btns[EV_SCREEN_COUNT];
static lv_obj_t * s_nav_icons_lbl[EV_SCREEN_COUNT];  /* the icon label per button */
static lv_obj_t * s_status_bar;
static lv_obj_t * s_source_label;
static lv_obj_t * s_status_label;
static lv_obj_t * s_alert_banner;
static lv_obj_t * s_alert_label;
static ev_screen_id_t s_active = EV_SCREEN_DASHBOARD;
static ev_battery_model_t s_model;
static ev_alert_state_t s_alert_state;
static ev_estimator_t s_estimator;
static ev_battery_state_t s_last_state;   /* whatever the UI last showed */
#if EV_ENABLE_SENSORS
static ev_live_source_t s_live;
static ev_battery_state_t s_live_state;
static bool s_using_live = false;
#endif
/* Same simulated dt as ev_battery_model_step (SIMULATION_SPEED * 1 s tick). */
#define EV_UI_SIM_DT_S 6.0f

static const char * s_nav_icons[EV_SCREEN_COUNT] = {
    LV_SYMBOL_HOME,
    LV_SYMBOL_REFRESH,
    LV_SYMBOL_CHARGE,
    LV_SYMBOL_LIST,
    LV_SYMBOL_SETTINGS
};

static lv_color_t quality_color(ev_data_quality_t quality)
{
    if (quality == EV_DATA_QUALITY_FAULT) return EV_COLOR_DANGER;
    if (quality == EV_DATA_QUALITY_WARNING) return EV_COLOR_WARN;
    return EV_COLOR_ACCENT;
}

static void play_alert_tone(ev_alert_kind_t kind)
{
    switch (kind) {
        case EV_ALERT_SENSOR_FAULT:
            ev_platform_play_alert_tone(2400U, 180U);
            break;
        case EV_ALERT_THERMAL_CRITICAL:
            ev_platform_play_alert_tone(2200U, 180U);
            break;
        case EV_ALERT_LOW_SOC_CRITICAL:
            ev_platform_play_alert_tone(1800U, 180U);
            break;
        case EV_ALERT_THERMAL_WARNING:
            ev_platform_play_alert_tone(1400U, 120U);
            break;
        case EV_ALERT_LOW_SOC_WARNING:
            ev_platform_play_alert_tone(1100U, 120U);
            break;
        case EV_ALERT_NONE:
        default:
            break;
    }
}

static void evaluate_alerts(const ev_battery_state_t * state, uint32_t real_elapsed_ms)
{
    ev_alert_state_update(&s_alert_state, state, real_elapsed_ms);
    const ev_alert_snapshot_t * alert = ev_alert_state_get_snapshot(&s_alert_state);
    if (alert != NULL && alert->audio_due) {
        play_alert_tone(alert->kind);
    }
}

static void render_status_bar(
    const ev_battery_state_t * state,
    const ev_alert_snapshot_t * alert
)
{
    /* Source badge: make LIVE vs SIM unmistakable. The pack descriptor matches
     * the actual source -- the real 3S Li-ion pack when live, the CALCE cell
     * dataset when replaying, or a plain "demo" for the simulation (we do NOT
     * print a misleading 48 V figure, since the product pack is 3S). LIVE is
     * shown in the accent colour so it reads at a glance; sim stays muted. */
    const bool is_live = state->source == EV_DATA_SOURCE_LIVE_BENCH;
    const char * pack_desc = is_live ? "3S Li-ion"
                           : (state->source == EV_DATA_SOURCE_DATASET ? "CALCE data"
                                                                      : "demo");
    lv_label_set_text_fmt(s_source_label, "%s | %s",
                          ev_data_source_name(state->source), pack_desc);
    lv_obj_set_style_text_color(s_source_label,
                                is_live ? EV_COLOR_ACCENT : EV_COLOR_SUBTEXT, 0);
    lv_label_set_text_fmt(s_status_label, "%s | %s",
                          ev_scenario_name(state->scenario),
                          ev_data_quality_name(state->quality));
    lv_obj_set_style_text_color(s_status_label, quality_color(state->quality), 0);

    if (alert == NULL || !alert->active) {
        lv_obj_remove_flag(s_source_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_alert_banner, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_add_flag(s_source_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_alert_banner, LV_OBJ_FLAG_HIDDEN);

    const bool warning = alert->severity == EV_ALERT_SEVERITY_WARNING;
    lv_obj_set_style_bg_color(s_alert_banner, warning ? EV_COLOR_WARN : EV_COLOR_DANGER, 0);
    lv_obj_set_style_text_color(s_alert_label, warning ? EV_COLOR_BG : EV_COLOR_TEXT, 0);

    switch (alert->kind) {
        case EV_ALERT_SENSOR_FAULT:
            lv_label_set_text(s_alert_label, "SENSOR DATA FAULT");
            break;
        case EV_ALERT_THERMAL_CRITICAL:
            lv_label_set_text_fmt(s_alert_label, "THERMAL CRIT | %.1fC", state->temperature_c);
            break;
        case EV_ALERT_LOW_SOC_CRITICAL:
            lv_label_set_text_fmt(s_alert_label, "LOW SOC CRIT | %.1f%%", state->soc_pct);
            break;
        case EV_ALERT_THERMAL_WARNING:
            lv_label_set_text_fmt(s_alert_label, "THERMAL WARN | %.1fC", state->temperature_c);
            break;
        case EV_ALERT_LOW_SOC_WARNING:
            lv_label_set_text_fmt(s_alert_label, "LOW SOC WARN | %.1f%%", state->soc_pct);
            break;
        case EV_ALERT_NONE:
        default:
            lv_label_set_text(s_alert_label, "");
            break;
    }
}

static void render_state(const ev_battery_state_t * state)
{
    if (state == NULL) return;

    render_status_bar(state, ev_alert_state_get_snapshot(&s_alert_state));
    ev_screen_dashboard_update(state);
    ev_screen_health_update(state);
    ev_screen_charging_update(state);
    ev_screen_trips_update(state);
}

/* Resolve this tick's state: live bench sensors if present, else simulation.
 * The estimator step uses the correct dt for whichever source is active. */
static const ev_battery_state_t * resolve_state(float * out_dt_s)
{
#if EV_ENABLE_SENSORS
    ev_sensor_reading_t reading;
    ev_sensors_read(&reading);
#if !EV_SENSORS_DETECT_ONLY
    /* Detect-only mode keeps the UI on the simulation while we validate wiring;
     * live switchover is enabled once inputs are connected and calibrated. */
    if (ev_live_source_update(&s_live, &reading, &s_live_state)) {
        *out_dt_s = 1.0f;  /* real time: the tick is 1 s of wall clock */
        /* On the FIRST live tick, re-seed + reconfigure the estimator for the
         * live pack. The boot seed/geometry was the 13S sim pack, which would
         * make the voltage model predict ~48 V and rail the SoC to 0 % for our
         * ~11 V (3S) bench pack. Seed from the live SoC and set 3S geometry so
         * the EKF tracks a real number. */
        if (!s_using_live) {
            ev_estimator_configure_pack(&s_estimator, EV_LIVE_PACK_CELLS_S,
                                        s_live.capacity_ah);
            ev_estimator_init(&s_estimator, s_live_state.soc_pct);
            ev_estimator_configure_pack(&s_estimator, EV_LIVE_PACK_CELLS_S,
                                        s_live.capacity_ah);  /* init resets geometry */
        }
        s_using_live = true;
        return &s_live_state;
    }
#endif
#endif

    /* Fallback (and default until sensors enabled): the simulation. */
    ev_battery_model_step(&s_model, 1000U);
    *out_dt_s = EV_UI_SIM_DT_S;
#if EV_ENABLE_SENSORS
    s_using_live = false;
#endif
    return ev_battery_model_get_state(&s_model);
}

static void controller_timer_cb(lv_timer_t * timer)
{
    LV_UNUSED(timer);
    float dt_s = EV_UI_SIM_DT_S;
    const ev_battery_state_t * state = resolve_state(&dt_s);
    s_last_state = *state;
    ev_estimator_step(&s_estimator, state, dt_s);
    evaluate_alerts(state, 1000U);
    render_state(state);

    /* Auto theme: if the RTC crossed the day/night boundary, the concrete
     * palette changes and we rebuild so the whole UI follows. No-op unless the
     * mode is AUTO and the light/dark choice actually flipped. */
    if (ev_theme_refresh()) {
        ev_ui_rebuild();
    }
}

void ev_ui_get_estimator_snapshot(ev_estimator_snapshot_t * snapshot)
{
    ev_estimator_get_snapshot_with_state(&s_estimator, &s_last_state, snapshot);
}

bool ev_ui_get_battery_state(ev_battery_state_t * state)
{
    if (state == NULL) return false;
    *state = s_last_state;
    return true;
}

void ev_ui_request_wifi_scan(void)
{
    ev_navigation_runtime_request_wifi_scan();
}

void ev_ui_zero_current(void)
{
    ev_sensors_zero_current();
}

bool ev_ui_get_wifi_status(ev_wifi_status_t * status)
{
    return ev_navigation_runtime_get_wifi_status(status);
}

void ev_ui_set_scenario(ev_scenario_t scenario)
{
    ev_battery_model_set_scenario(&s_model, scenario);
    const ev_battery_state_t * state = ev_battery_model_get_state(&s_model);
    /* Reseed with a deliberate 10 pp bias so the LEARNING -> READY
     * convergence is visible after every scenario switch. */
    ev_estimator_init(&s_estimator, state != NULL ? state->soc_pct : -1.0f);
    evaluate_alerts(state, 0U);
    render_state(state);
}

void ev_ui_set_navigation_snapshot(const ev_navigation_snapshot_t * navigation)
{
    ev_screen_charging_navigation_update(navigation);
}

bool ev_ui_take_route_request(uint8_t * station_index)
{
    return ev_screen_charging_take_route_request(station_index);
}

bool ev_ui_take_refresh_request(void)
{
    return ev_screen_charging_take_refresh_request();
}

static void nav_btn_event_cb(lv_event_t * event)
{
    ev_screen_id_t id = (ev_screen_id_t)(intptr_t)lv_event_get_user_data(event);
    ev_ui_show_screen(id);
}

void ev_ui_show_screen(ev_screen_id_t id)
{
    if (id >= EV_SCREEN_COUNT) return;

    for (int i = 0; i < EV_SCREEN_COUNT; i++) {
        if (s_screens[i] == NULL) continue;
        if (i == id) lv_obj_remove_flag(s_screens[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_screens[i], LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < EV_SCREEN_COUNT; i++) {
        if (s_nav_btns[i] == NULL) continue;
        if (i == id) {
            lv_obj_set_style_bg_color(s_nav_btns[i], EV_COLOR_ACCENT, 0);
            /* High-contrast icon on the accent fill so the selected icon stays
             * clearly visible. Set on the LABEL: its own text-colour style
             * overrides anything set on the parent button (this was the bug that
             * left the selected icon washed out inside the green circle). */
            if (s_nav_icons_lbl[i] != NULL) {
                lv_obj_set_style_text_color(s_nav_icons_lbl[i], EV_COLOR_ON_ACCENT, 0);
            }
        } else {
            lv_obj_set_style_bg_color(s_nav_btns[i], EV_COLOR_CARD, 0);
            if (s_nav_icons_lbl[i] != NULL) {
                lv_obj_set_style_text_color(s_nav_icons_lbl[i], EV_COLOR_SUBTEXT, 0);
            }
        }
    }

    s_active = id;
}

static void build_status_bar(lv_obj_t * parent)
{
    s_status_bar = lv_obj_create(parent);
    lv_obj_remove_style_all(s_status_bar);
    lv_obj_set_size(s_status_bar, EV_DISP_W, EV_STATUS_HEIGHT);
    lv_obj_align(s_status_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(s_status_bar, EV_COLOR_STATUSBAR, 0);
    lv_obj_set_style_bg_opa(s_status_bar, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_status_bar, LV_OBJ_FLAG_SCROLLABLE);

    s_source_label = lv_label_create(s_status_bar);
    lv_obj_set_style_text_font(s_source_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_source_label, EV_COLOR_SUBTEXT, 0);
    lv_obj_align(s_source_label, LV_ALIGN_LEFT_MID, 4, 0);

    s_status_label = lv_label_create(s_status_bar);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_12, 0);
    lv_obj_align(s_status_label, LV_ALIGN_RIGHT_MID, -4, 0);

    s_alert_banner = lv_obj_create(s_status_bar);
    lv_obj_remove_style_all(s_alert_banner);
    lv_obj_set_size(s_alert_banner, EV_DISP_W, EV_STATUS_HEIGHT);
    lv_obj_center(s_alert_banner);
    lv_obj_set_style_bg_opa(s_alert_banner, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_alert_banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_alert_banner, LV_OBJ_FLAG_HIDDEN);

    s_alert_label = lv_label_create(s_alert_banner);
    lv_obj_set_style_text_font(s_alert_label, &lv_font_montserrat_12, 0);
    lv_obj_center(s_alert_label);
}

static void build_nav_bar(lv_obj_t * parent)
{
    lv_obj_t * bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), EV_NAV_HEIGHT);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, EV_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < EV_SCREEN_COUNT; i++) {
        lv_obj_t * button = lv_button_create(bar);
        lv_obj_set_size(button, 32, 32);
        lv_obj_set_style_radius(button, 16, 0);
        lv_obj_set_style_bg_color(button, EV_COLOR_CARD, 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_set_style_pad_all(button, 0, 0);

        lv_obj_t * label = lv_label_create(button);
        lv_label_set_text(label, s_nav_icons[i]);
        lv_obj_set_style_text_color(label, EV_COLOR_SUBTEXT, 0);
        lv_obj_center(label);

        lv_obj_add_event_cb(button, nav_btn_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_nav_btns[i] = button;
        s_nav_icons_lbl[i] = label;  /* colour must be set on the LABEL, not the
                                      * button: the label's own text-colour style
                                      * overrides anything inherited. */
    }
}

/* Build (or rebuild) all themed widgets on `screen`: status bar, content area
 * with the five screens, and the nav bar. Kept separate from ev_ui_init so a
 * theme switch can tear these down and recreate them with the new palette,
 * without disturbing the model/estimator/alert state. */
static void build_ui_widgets(lv_obj_t * screen)
{
    lv_obj_set_style_bg_color(screen, EV_COLOR_BG, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    build_status_bar(screen);

    s_content_area = lv_obj_create(screen);
    lv_obj_remove_style_all(s_content_area);
    lv_obj_set_size(s_content_area, EV_DISP_W, EV_CONTENT_H);
    lv_obj_align(s_content_area, LV_ALIGN_TOP_MID, 0, EV_STATUS_HEIGHT);
    lv_obj_set_style_bg_color(s_content_area, EV_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_content_area, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_content_area, LV_OBJ_FLAG_SCROLLABLE);

    s_screens[EV_SCREEN_DASHBOARD] = ev_screen_dashboard_create(s_content_area);
    s_screens[EV_SCREEN_HEALTH] = ev_screen_health_create(s_content_area);
    s_screens[EV_SCREEN_CHARGING] = ev_screen_charging_create(s_content_area);
    s_screens[EV_SCREEN_TRIPS] = ev_screen_trips_create(s_content_area);
    s_screens[EV_SCREEN_SETTINGS] = ev_screen_settings_create(s_content_area);

    build_nav_bar(screen);
}

void ev_ui_init(void)
{
    lv_obj_t * screen = lv_screen_active();

    ev_battery_model_init(&s_model);
    ev_alert_state_init(&s_alert_state);
#if EV_ENABLE_SENSORS
    ev_sensors_init();
    ev_live_source_init(&s_live, 2.5f);  /* bench pack ~2.5 Ah */
#endif
    {
        const ev_battery_state_t * boot_state = ev_battery_model_get_state(&s_model);
        ev_estimator_init(&s_estimator, boot_state != NULL ? boot_state->soc_pct : -1.0f);
        if (boot_state != NULL) s_last_state = *boot_state;
    }

    build_ui_widgets(screen);

    const ev_battery_state_t * state = ev_battery_model_get_state(&s_model);
    evaluate_alerts(state, 0U);
    render_state(state);
    ev_ui_show_screen(EV_SCREEN_DASHBOARD);
    lv_timer_create(controller_timer_cb, 1000U, NULL);
}

/* Rebuild the whole UI with the current theme palette. Called after a theme
 * switch. Deletes the existing status bar, content area (which owns all five
 * screens), and nav bar, then recreates them, restores the active screen, and
 * re-renders the last state. Model/estimator/alert state is untouched.
 *
 * Runs on the LVGL thread (from a Settings callback), so there is no race with
 * the controller timer — it cannot fire mid-rebuild. */
void ev_ui_rebuild(void)
{
    lv_obj_t * screen = lv_screen_active();
    const ev_screen_id_t previously_active = s_active;

    /* Everything we build (status bar, content area with all five screens, nav
     * bar) is a child of the active screen and nothing else lives there, so a
     * single lv_obj_clean() tears the whole UI down. The Wi-Fi/log status
     * timers in Settings are GLOBAL lv_timers (created once, guarded) and are
     * intentionally NOT deleted here; their callbacks null-check the statics,
     * which build_ui_widgets() repoints to the freshly-created widgets. */
    lv_obj_clean(screen);
    s_status_bar = NULL;
    s_content_area = NULL;
    for (int i = 0; i < EV_SCREEN_COUNT; i++) {
        s_screens[i] = NULL;
        s_nav_btns[i] = NULL;
        s_nav_icons_lbl[i] = NULL;
    }

    build_ui_widgets(screen);

    render_state(&s_last_state);
    ev_ui_show_screen(previously_active);
}
