#include "ui.h"

/* Page 4 — live Session / Energy monitor.
 *
 * Replaces the old trip/weekly view (which relied on an odometer and multi-day
 * history the bench rig does not have) with quantities the device genuinely
 * measures this power-up: energy discharged/charged, peak power, a live power
 * trend, and session time. Everything here is real, integrated from the
 * calibrated pack V x I -- so the page comes alive the moment a load is applied.
 */

static lv_obj_t * s_out_value;      /* energy discharged (Wh) */
static lv_obj_t * s_in_value;       /* energy charged (Wh) */
static lv_obj_t * s_peak_value;     /* peak power (W) */
static lv_obj_t * s_power_chart;    /* live instantaneous power trend */
static lv_chart_series_t * s_power_series;
static lv_obj_t * s_session_label;  /* elapsed session time + live power */

#define POWER_CHART_POINTS 30       /* ~30 s of history at the 1 Hz tick */

void ev_screen_trips_update(const ev_battery_state_t * state)
{
    if (state == NULL) return;

    if (!state->sensor_data_valid) {
        lv_label_set_text(s_out_value, "-- Wh");
        lv_label_set_text(s_in_value, "-- Wh");
        lv_label_set_text(s_peak_value, "-- W");
        lv_label_set_text(s_session_label, "Waiting for sensor data");
        return;
    }

    lv_label_set_text_fmt(s_out_value, "%.1f Wh", (double)state->session_wh_out);
    lv_label_set_text_fmt(s_in_value, "%.1f Wh", (double)state->session_wh_in);
    lv_label_set_text_fmt(s_peak_value, "%.0f W", (double)state->session_peak_power_w);

    /* Live instantaneous |power| pushed onto the trend (scrolls right-to-left).
     * Charted in whole watts; a 21 W bulb reads ~18 W, idle ~0. */
    const float p_w = state->voltage_v *
                      (state->current_a < 0.0f ? -state->current_a : state->current_a);
    lv_chart_set_next_value(s_power_chart, s_power_series, (int32_t)(p_w + 0.5f));

    /* Session time as mm:ss, plus the live power for an at-a-glance number. */
    const uint32_t secs = state->session_seconds;
    lv_label_set_text_fmt(s_session_label, "Session %lu:%02lu  |  now %.1f W",
                          (unsigned long)(secs / 60U), (unsigned long)(secs % 60U),
                          (double)p_w);
}

lv_obj_t * ev_screen_trips_create(lv_obj_t * parent)
{
    lv_obj_t * root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, EV_DISP_W, EV_CONTENT_H);
    lv_obj_set_style_pad_all(root, 6, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(root, 4, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    /* Title */
    lv_obj_t * title = lv_label_create(root);
    lv_label_set_text(title, "Session energy (live)");
    lv_obj_set_style_text_color(title, EV_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);

    /* Three-metric card: Out / In / Peak */
    lv_obj_t * card = lv_obj_create(root);
    lv_obj_set_size(card, 308, 48);
    lv_obj_set_style_bg_color(card, EV_COLOR_CARD, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    const char * labels[3] = {"Out", "In", "Peak"};
    lv_obj_t ** values[3] = {&s_out_value, &s_in_value, &s_peak_value};
    for (int i = 0; i < 3; i++) {
        lv_obj_t * column = lv_obj_create(card);
        lv_obj_remove_style_all(column);
        lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(column, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(column, LV_OBJ_FLAG_SCROLLABLE);

        *values[i] = lv_label_create(column);
        lv_label_set_text(*values[i], "-- ");
        lv_obj_set_style_text_color(*values[i], EV_COLOR_TEXT, 0);
        lv_obj_set_style_text_font(*values[i], &lv_font_montserrat_14, 0);

        lv_obj_t * label = lv_label_create(column);
        lv_label_set_text(label, labels[i]);
        lv_obj_set_style_text_color(label, EV_COLOR_SUBTEXT, 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    }

    /* Live power trend chart (instantaneous watts). */
    lv_obj_t * chart_title = lv_label_create(root);
    lv_label_set_text(chart_title, "Power (W) - live");
    lv_obj_set_style_text_color(chart_title, EV_COLOR_SUBTEXT, 0);
    lv_obj_set_style_text_font(chart_title, &lv_font_montserrat_12, 0);

    s_power_chart = lv_chart_create(root);
    lv_obj_set_size(s_power_chart, 308, 74);
    lv_chart_set_type(s_power_chart, LV_CHART_TYPE_LINE);
    /* 0..30 W covers the 21 W bulb with headroom; auto-clamps above. */
    lv_chart_set_axis_range(s_power_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 30);
    lv_chart_set_point_count(s_power_chart, POWER_CHART_POINTS);
    lv_chart_set_update_mode(s_power_chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_set_style_bg_color(s_power_chart, EV_COLOR_CARD, 0);
    lv_obj_set_style_border_width(s_power_chart, 0, 0);
    lv_obj_set_style_radius(s_power_chart, 8, 0);
    lv_obj_set_style_pad_all(s_power_chart, 6, 0);
    lv_obj_set_style_size(s_power_chart, 0, 0, LV_PART_INDICATOR);  /* hide dots */
    s_power_series = lv_chart_add_series(s_power_chart, EV_COLOR_ACCENT, LV_CHART_AXIS_PRIMARY_Y);

    /* Session time + live power line. */
    s_session_label = lv_label_create(root);
    lv_label_set_text(s_session_label, "Session 0:00");
    lv_obj_set_style_text_color(s_session_label, EV_COLOR_SUBTEXT, 0);
    lv_obj_set_style_text_font(s_session_label, &lv_font_montserrat_12, 0);

    return root;
}
