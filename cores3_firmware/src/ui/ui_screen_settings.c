#include "ui.h"
#include "platform/ev_logger.h"
#include "platform/ev_power.h"

#include <stdio.h>
#include <string.h>

static lv_obj_t * s_wifi_connected_label;
static lv_obj_t * s_wifi_list;
static lv_obj_t * s_log_label;
static uint32_t s_wifi_last_revision = UINT32_MAX;
/* The Wi-Fi/log status timers are global and persist across screen rebuilds
 * (theme switches recreate the Settings screen); create them only once. */
static bool s_timers_created = false;

static void zero_current_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    ev_ui_zero_current();  /* captures the ACS723 idle voltage as 0 A */
}

static void brightness_changed_cb(lv_event_t * event)
{
    lv_obj_t * slider = lv_event_get_target_obj(event);
    const int32_t pct = lv_slider_get_value(slider);  /* 10..100 */
    /* Map 10..100 % to a usable backlight range (avoid fully dark). */
    const uint8_t b = (uint8_t)((pct * 255) / 100);
    ev_power_set_active_brightness(b);
}

static void saver_switch_cb(lv_event_t * event)
{
    lv_obj_t * sw = lv_event_get_target_obj(event);
    ev_power_set_saver_enabled(lv_obj_has_state(sw, LV_STATE_CHECKED));
}

static void theme_changed_cb(lv_event_t * event)
{
    lv_obj_t * dd = lv_event_get_target_obj(event);
    /* Dropdown order matches ev_theme_mode_t: 0=Dark, 1=Light, 2=Auto. */
    ev_theme_set_mode((ev_theme_mode_t)lv_dropdown_get_selected(dd));
    /* Rebuild every screen so all widgets pick up the new palette. This runs
     * on the LVGL thread; the current (Settings) screen is torn down inside,
     * so do no further work with `dd` after this call. */
    ev_ui_rebuild();
}

static void log_switch_cb(lv_event_t * event)
{
    lv_obj_t * sw = lv_event_get_target_obj(event);
    ev_logger_set_enabled(lv_obj_has_state(sw, LV_STATE_CHECKED));
}

static void log_status_timer_cb(lv_timer_t * timer)
{
    LV_UNUSED(timer);
    if (s_log_label == NULL) return;
    ev_logger_status_t st;
    ev_logger_get_status(&st);
    if (!st.active) {
        lv_label_set_text(s_log_label, "Log: unavailable");
        lv_obj_set_style_text_color(s_log_label, EV_COLOR_DANGER, 0);
        return;
    }
    if (!st.enabled) {
        lv_label_set_text_fmt(s_log_label, "%s | paused | %lu rows",
                              ev_logger_backend_name(st.backend),
                              (unsigned long)st.rows_logged);
        lv_obj_set_style_text_color(s_log_label, EV_COLOR_SUBTEXT, 0);
        return;
    }
    /* e.g. "internal flash | 428 rows | 18.3 KB". */
    lv_label_set_text_fmt(s_log_label, "%s | %lu rows | %.1f KB",
                          ev_logger_backend_name(st.backend),
                          (unsigned long)st.rows_logged,
                          (double)st.bytes_written / 1024.0);
    lv_obj_set_style_text_color(s_log_label, EV_COLOR_ACCENT, 0);
}

static void scenario_changed_cb(lv_event_t * event)
{
    lv_obj_t * dropdown = lv_event_get_target_obj(event);
    ev_ui_set_scenario((ev_scenario_t)lv_dropdown_get_selected(dropdown));
}

static void wifi_scan_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    ev_ui_request_wifi_scan();
    if (s_wifi_list != NULL) {
        lv_label_set_text(s_wifi_list, "Scanning...");
        lv_obj_set_style_text_color(s_wifi_list, EV_COLOR_WARN, 0);
    }
}

/* Four-level signal label from RSSI (dBm). */
static const char * signal_bars(int8_t rssi)
{
    if (rssi >= -60) return "||||";
    if (rssi >= -70) return "||| ";
    if (rssi >= -80) return "||  ";
    return "|   ";
}

static void render_wifi_status(const ev_wifi_status_t * status)
{
    if (status->connected) {
        lv_label_set_text_fmt(s_wifi_connected_label, LV_SYMBOL_WIFI " %s  %ddBm",
                              status->connected_ssid, (int)status->connected_rssi);
        lv_obj_set_style_text_color(s_wifi_connected_label, EV_COLOR_ACCENT, 0);
    } else {
        lv_label_set_text(s_wifi_connected_label, LV_SYMBOL_WIFI " not connected");
        lv_obj_set_style_text_color(s_wifi_connected_label, EV_COLOR_SUBTEXT, 0);
    }

    if (status->scanning) {
        lv_label_set_text(s_wifi_list, "Scanning...");
        lv_obj_set_style_text_color(s_wifi_list, EV_COLOR_WARN, 0);
        return;
    }
    if (status->scan_count == 0U) {
        lv_label_set_text(s_wifi_list, "Tap Scan to list nearby networks");
        lv_obj_set_style_text_color(s_wifi_list, EV_COLOR_SUBTEXT, 0);
        return;
    }

    /* Build a compact multi-line list; a star marks configured networks. */
    char buffer[512];
    size_t used = 0U;
    for (uint8_t i = 0U; i < status->scan_count && used < sizeof(buffer) - 48U; ++i) {
        const ev_wifi_scan_entry_t * e = &status->entries[i];
        used += (size_t)snprintf(buffer + used, sizeof(buffer) - used,
                                 "%s %-16.16s %s%s\n",
                                 signal_bars(e->rssi), e->ssid,
                                 e->known ? LV_SYMBOL_OK " " : "",
                                 e->open ? "open" : "");
    }
    if (used > 0U && buffer[used - 1U] == '\n') buffer[used - 1U] = '\0';
    lv_label_set_text(s_wifi_list, buffer);
    lv_obj_set_style_text_color(s_wifi_list, EV_COLOR_TEXT, 0);
}

static void wifi_status_timer_cb(lv_timer_t * timer)
{
    LV_UNUSED(timer);
    if (s_wifi_connected_label == NULL || s_wifi_list == NULL) return;
    ev_wifi_status_t status;
    if (!ev_ui_get_wifi_status(&status)) return;
    if (status.revision == s_wifi_last_revision) return;
    s_wifi_last_revision = status.revision;
    render_wifi_status(&status);
}

lv_obj_t * ev_screen_settings_create(lv_obj_t * parent)
{
    lv_obj_t * root = lv_obj_create(parent);
    lv_obj_set_size(root, EV_DISP_W, EV_CONTENT_H);
    lv_obj_set_style_bg_color(root, EV_COLOR_BG, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 6, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(root, 4, 0);
    lv_obj_add_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(root, LV_DIR_VER);

    /* Battery config: honest read-only summary of the bench pack the firmware is
     * configured for. (The earlier editable Volts/Ah/chemistry + Vehicle-profile
     * fields were non-functional stubs and have been removed so Settings only
     * shows controls that actually do something.) */
    lv_obj_t * sec1 = lv_label_create(root);
    lv_label_set_text(sec1, "Battery");
    lv_obj_set_style_text_color(sec1, EV_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(sec1, &lv_font_montserrat_12, 0);

    lv_obj_t * batt_info = lv_label_create(root);
    lv_label_set_text(batt_info, "3S Li-ion NMC pack, ~2.5 Ah (12.6 V max)");
    lv_obj_set_width(batt_info, 308);
    lv_label_set_long_mode(batt_info, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(batt_info, EV_COLOR_SUBTEXT, 0);
    lv_obj_set_style_text_font(batt_info, &lv_font_montserrat_12, 0);

    lv_obj_t * sec3 = lv_label_create(root);
    lv_label_set_text(sec3, "Wi-Fi");
    lv_obj_set_style_text_color(sec3, EV_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(sec3, &lv_font_montserrat_12, 0);

    lv_obj_t * wifi_row = lv_obj_create(root);
    lv_obj_remove_style_all(wifi_row);
    lv_obj_set_size(wifi_row, 308, 24);
    lv_obj_set_flex_flow(wifi_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wifi_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(wifi_row, LV_OBJ_FLAG_SCROLLABLE);

    s_wifi_connected_label = lv_label_create(wifi_row);
    lv_label_set_text(s_wifi_connected_label, LV_SYMBOL_WIFI " not connected");
    lv_obj_set_width(s_wifi_connected_label, 236);
    lv_label_set_long_mode(s_wifi_connected_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_color(s_wifi_connected_label, EV_COLOR_SUBTEXT, 0);
    lv_obj_set_style_text_font(s_wifi_connected_label, &lv_font_montserrat_12, 0);

    lv_obj_t * scan_btn = lv_button_create(wifi_row);
    lv_obj_set_size(scan_btn, 60, 22);
    lv_obj_set_style_pad_all(scan_btn, 0, 0);
    lv_obj_set_style_radius(scan_btn, 6, 0);
    lv_obj_set_style_bg_color(scan_btn, EV_COLOR_CARD, 0);
    lv_obj_set_style_shadow_width(scan_btn, 0, 0);
    lv_obj_add_event_cb(scan_btn, wifi_scan_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * scan_lbl = lv_label_create(scan_btn);
    lv_label_set_text(scan_lbl, "Scan");
    lv_obj_set_style_text_color(scan_lbl, EV_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(scan_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(scan_lbl);

    /* Network list populated by the periodic status poll below. */
    s_wifi_list = lv_label_create(root);
    lv_label_set_text(s_wifi_list, "Tap Scan to list nearby networks");
    lv_obj_set_width(s_wifi_list, 308);
    lv_obj_set_style_text_color(s_wifi_list, EV_COLOR_SUBTEXT, 0);
    lv_obj_set_style_text_font(s_wifi_list, &lv_font_montserrat_12, 0);

    /* Poll the worker's Wi-Fi status a few times a second on the LVGL thread.
     * Created ONCE: the screen may be recreated on a theme switch, but these
     * are global timers that read the (repointed) statics, so we must not
     * duplicate them each rebuild. Their callbacks null-check the statics. */
    if (!s_timers_created) {
        lv_timer_create(wifi_status_timer_cb, 700, NULL);
    }

    /* Alerts: show the ACTIVE thresholds the alert engine uses (read-only). The
     * earlier temp/SoC sliders implied adjustable thresholds but were not wired
     * to the engine, so they are replaced with an honest summary of the fixed
     * thresholds (kept in sync with ev_alert_state.c). */
    lv_obj_t * sec4 = lv_label_create(root);
    lv_label_set_text(sec4, "Alerts (thresholds)");
    lv_obj_set_style_text_color(sec4, EV_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(sec4, &lv_font_montserrat_12, 0);

    lv_obj_t * alerts_info = lv_label_create(root);
    lv_label_set_text(alerts_info,
                      "Temp: warn 45°C / crit 50°C\nLow SoC: warn 15% / crit 8%");
    lv_obj_set_width(alerts_info, 308);
    lv_label_set_long_mode(alerts_info, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(alerts_info, EV_COLOR_SUBTEXT, 0);
    lv_obj_set_style_text_font(alerts_info, &lv_font_montserrat_12, 0);

    lv_obj_t * sec5 = lv_label_create(root);
    lv_label_set_text(sec5, "Display");
    lv_obj_set_style_text_color(sec5, EV_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(sec5, &lv_font_montserrat_12, 0);

    lv_obj_t * brightness_row = lv_obj_create(root);
    lv_obj_remove_style_all(brightness_row);
    lv_obj_set_size(brightness_row, 308, 22);
    lv_obj_set_flex_flow(brightness_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brightness_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(brightness_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * brightness_label = lv_label_create(brightness_row);
    lv_label_set_text(brightness_label, "Brightness");
    lv_obj_set_style_text_color(brightness_label, EV_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(brightness_label, &lv_font_montserrat_12, 0);

    lv_obj_t * brightness_slider = lv_slider_create(brightness_row);
    lv_obj_set_size(brightness_slider, 140, 10);
    lv_slider_set_range(brightness_slider, 10, 100);
    lv_slider_set_value(brightness_slider, 50, LV_ANIM_OFF);  /* ~128/255 default */
    lv_obj_set_style_bg_color(brightness_slider, EV_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_add_event_cb(brightness_slider, brightness_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* --- Theme: Dark / Light / Auto (day-night by RTC) --- */
    lv_obj_t * theme_row = lv_obj_create(root);
    lv_obj_remove_style_all(theme_row);
    lv_obj_set_size(theme_row, 308, 28);
    lv_obj_set_flex_flow(theme_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(theme_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(theme_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * theme_label = lv_label_create(theme_row);
    lv_label_set_text(theme_label, "Theme");
    lv_obj_set_style_text_color(theme_label, EV_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(theme_label, &lv_font_montserrat_12, 0);

    lv_obj_t * theme_dd = lv_dropdown_create(theme_row);
    lv_dropdown_set_options(theme_dd, "Dark\nLight\nAuto (day/night)");
    lv_obj_set_size(theme_dd, 170, 26);
    lv_obj_set_style_text_font(theme_dd, &lv_font_montserrat_12, 0);
    lv_dropdown_set_selected(theme_dd, (uint16_t)ev_theme_get_mode());
    lv_obj_add_event_cb(theme_dd, theme_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* --- Power management (AXP2101 + RTC): idle power-saver toggle --- */
    lv_obj_t * pwr_head = lv_obj_create(root);
    lv_obj_remove_style_all(pwr_head);
    lv_obj_set_size(pwr_head, 308, 24);
    lv_obj_set_flex_flow(pwr_head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pwr_head, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(pwr_head, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * pwr_sec = lv_label_create(pwr_head);
    lv_label_set_text(pwr_sec, "Power saver");
    lv_obj_set_style_text_color(pwr_sec, EV_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(pwr_sec, &lv_font_montserrat_12, 0);

    lv_obj_t * saver_switch = lv_switch_create(pwr_head);
    lv_obj_set_size(saver_switch, 40, 22);
    lv_obj_set_style_bg_color(saver_switch, EV_COLOR_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (ev_power_saver_enabled()) lv_obj_add_state(saver_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(saver_switch, saver_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t * pwr_note = lv_label_create(root);
    lv_label_set_text(pwr_note, "Dims then sleeps when idle; tap to wake. "
                                "Hold side POWER btn to shut down.");
    lv_obj_set_width(pwr_note, 308);
    lv_label_set_long_mode(pwr_note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(pwr_note, EV_COLOR_SUBTEXT, 0);
    lv_obj_set_style_text_font(pwr_note, &lv_font_montserrat_12, 0);

    lv_obj_t * log_head = lv_obj_create(root);
    lv_obj_remove_style_all(log_head);
    lv_obj_set_size(log_head, 308, 24);
    lv_obj_set_flex_flow(log_head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(log_head, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(log_head, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * sec6 = lv_label_create(log_head);
    lv_label_set_text(sec6, "Data log");
    lv_obj_set_style_text_color(sec6, EV_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(sec6, &lv_font_montserrat_12, 0);

    lv_obj_t * log_switch = lv_switch_create(log_head);
    lv_obj_set_size(log_switch, 40, 22);
    lv_obj_set_style_bg_color(log_switch, EV_COLOR_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (ev_logger_is_enabled()) lv_obj_add_state(log_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(log_switch, log_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_log_label = lv_label_create(root);
    lv_obj_set_width(s_log_label, 308);
    lv_label_set_text(s_log_label, "Log: starting...");
    lv_obj_set_style_text_color(s_log_label, EV_COLOR_SUBTEXT, 0);
    lv_obj_set_style_text_font(s_log_label, &lv_font_montserrat_12, 0);
    if (!s_timers_created) {
        lv_timer_create(log_status_timer_cb, 1500, NULL);
    }
    s_timers_created = true;

    /* --- Sensors: current zero calibration --- */
    lv_obj_t * cur_head = lv_obj_create(root);
    lv_obj_remove_style_all(cur_head);
    lv_obj_set_size(cur_head, 308, 26);
    lv_obj_set_flex_flow(cur_head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cur_head, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(cur_head, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * sec7 = lv_label_create(cur_head);
    lv_label_set_text(sec7, "Current sensor");
    lv_obj_set_style_text_color(sec7, EV_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(sec7, &lv_font_montserrat_12, 0);

    lv_obj_t * zero_btn = lv_button_create(cur_head);
    lv_obj_set_size(zero_btn, 96, 24);
    lv_obj_set_style_radius(zero_btn, 6, 0);
    lv_obj_set_style_bg_color(zero_btn, EV_COLOR_CARD, 0);
    lv_obj_set_style_shadow_width(zero_btn, 0, 0);
    lv_obj_add_event_cb(zero_btn, zero_current_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * zlbl = lv_label_create(zero_btn);
    lv_label_set_text(zlbl, "Zero (no load)");
    lv_obj_set_style_text_color(zlbl, EV_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(zlbl, &lv_font_montserrat_12, 0);
    lv_obj_center(zlbl);

    lv_obj_t * cur_note = lv_label_create(root);
    lv_label_set_text(cur_note, "Zero with the load OFF to null the offset");
    lv_obj_set_style_text_color(cur_note, EV_COLOR_SUBTEXT, 0);
    lv_obj_set_style_text_font(cur_note, &lv_font_montserrat_12, 0);

    /* --- Demo simulation (clearly separated from the live controls above) ---
     * Drives the shared model with deterministic scenarios so the UI, alerts and
     * audio can be shown on demand (e.g. a thermal or low-SoC alert) without
     * abusing the real pack. When live sensors are active the dashboard uses
     * them; this is for demonstration/testing. */
    lv_obj_t * sec8 = lv_label_create(root);
    lv_label_set_text(sec8, "Demo simulation");
    lv_obj_set_style_text_color(sec8, EV_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(sec8, &lv_font_montserrat_12, 0);

    lv_obj_t * scenario_dropdown = lv_dropdown_create(root);
    lv_dropdown_set_options(scenario_dropdown,
        "Normal discharge\nCharging\nHigh current\nThermal warning\nLow SoC\nSensor fault");
    lv_obj_set_size(scenario_dropdown, 308, 26);
    lv_obj_set_style_text_font(scenario_dropdown, &lv_font_montserrat_12, 0);
    lv_obj_add_event_cb(scenario_dropdown, scenario_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t * sim_note = lv_label_create(root);
    lv_label_set_text(sim_note, "Simulated scenario for demo/testing (not live data)");
    lv_obj_set_width(sim_note, 308);
    lv_label_set_long_mode(sim_note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(sim_note, EV_COLOR_SUBTEXT, 0);
    lv_obj_set_style_text_font(sim_note, &lv_font_montserrat_12, 0);

    lv_obj_t * spacer = lv_obj_create(root);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_size(spacer, 10, 10);
    return root;
}
