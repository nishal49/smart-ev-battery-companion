#include "ui.h"

static lv_obj_t * s_soc_arc;
static lv_obj_t * s_soc_label;
static lv_obj_t * s_range_label;
static lv_obj_t * s_current_label;
static lv_obj_t * s_current_arrow;
static lv_obj_t * s_voltage_label;
static lv_obj_t * s_temp_bar;
static lv_obj_t * s_temp_label;
static lv_obj_t * s_health_label;
static lv_obj_t * s_estimator_label;

static int rounded(float value)
{
    return (int)(value + 0.5f);
}

static void update_soc_color(float soc)
{
    lv_color_t color;
    if (soc > 55.0f) color = EV_COLOR_ACCENT;
    else if (soc > 25.0f) color = EV_COLOR_WARN;
    else color = EV_COLOR_DANGER;
    lv_obj_set_style_arc_color(s_soc_arc, color, LV_PART_INDICATOR);
}

static void update_estimator_readout(void)
{
    ev_estimator_snapshot_t est;
    ev_ui_get_estimator_snapshot(&est);

    lv_color_t colour = EV_COLOR_SUBTEXT;
    switch (est.state) {
        case EV_ESTIMATOR_READY:
            lv_label_set_text_fmt(s_estimator_label, "EKF %d%% " LV_SYMBOL_OK " ready",
                                  rounded(est.soc_pct));
            colour = EV_COLOR_ACCENT;
            break;
        case EV_ESTIMATOR_LEARNING:
            lv_label_set_text_fmt(s_estimator_label, "EKF %d%% settling",
                                  rounded(est.soc_pct));
            colour = EV_COLOR_WARN;
            break;
        case EV_ESTIMATOR_DERATED:
            lv_label_set_text_fmt(s_estimator_label, "EKF %d%% derated",
                                  rounded(est.soc_pct));
            colour = EV_COLOR_WARN;
            break;
        case EV_ESTIMATOR_UNAVAILABLE:
        default:
            lv_label_set_text(s_estimator_label, "EKF offline");
            colour = EV_COLOR_DANGER;
            break;
    }
    lv_obj_set_style_text_color(s_estimator_label, colour, 0);
}

void ev_screen_dashboard_update(const ev_battery_state_t * state)
{
    if (state == NULL) return;

    /* SoH is only meaningful after a full reference cycle, which a short bench
     * demo has not run -- state that honestly rather than implying a learning
     * process that completes. */
    lv_label_set_text(s_health_label, "SoH: n/a");
    lv_obj_set_style_text_color(s_health_label, EV_COLOR_SUBTEXT, 0);
    update_estimator_readout();

    if (!state->sensor_data_valid) {
        lv_arc_set_value(s_soc_arc, 0);
        lv_label_set_text(s_soc_label, "--");
        lv_label_set_text(s_range_label, "Range unavailable");
        lv_label_set_text(s_current_label, "-- A");
        lv_label_set_text(s_current_arrow, LV_SYMBOL_CLOSE);
        lv_obj_set_style_text_color(s_current_arrow, EV_COLOR_DANGER, 0);
        lv_label_set_text(s_voltage_label, "-- V");
        lv_bar_set_value(s_temp_bar, 15, LV_ANIM_OFF);
        lv_label_set_text(s_temp_label, "--°C");
        return;
    }

    const int soc = rounded(state->soc_pct);
    lv_arc_set_value(s_soc_arc, soc);
    update_soc_color(state->soc_pct);
    lv_label_set_text_fmt(s_soc_label, "%d%%", soc);
    lv_label_set_text_fmt(s_range_label, "%d-%d km range",
                          rounded(state->conservative_range_km),
                          rounded(state->nominal_range_km));

    const bool charging = state->scenario == EV_SCENARIO_CHARGING;
    float current_magnitude = state->current_a < 0.0f ? -state->current_a : state->current_a;
    /* Idle deadband: the ACS723's zero-point drifts slightly with the rail and
     * temperature, so ~0.1-0.2 A of residual can show at true zero load. Below
     * this band we display 0.0 A so an idle pack reads clean instead of jittery.
     * Matches the live source's charge/discharge deadband. */
    if (current_magnitude < 0.15f) current_magnitude = 0.0f;
    lv_label_set_text_fmt(s_current_label, "%.1f A", (double)current_magnitude);
    /* Charging: show a charge bolt in accent green (current flows into the pack).
     * Discharging: a down arrow. This makes the charge state unmistakable at a
     * glance, beyond just the current sign. */
    lv_label_set_text(s_current_arrow, charging ? LV_SYMBOL_CHARGE : LV_SYMBOL_DOWN);
    lv_obj_set_style_text_color(s_current_arrow,
                                charging ? EV_COLOR_ACCENT : EV_COLOR_WARN, 0);
    lv_label_set_text_fmt(s_voltage_label, "%.1f V", (double)state->voltage_v);

    if (state->temperature_c <= EV_TEMPERATURE_UNKNOWN + 1.0f) {
        /* No thermistor connected: show honest placeholder, not a fake value. */
        lv_bar_set_value(s_temp_bar, 15, LV_ANIM_OFF);
        lv_label_set_text(s_temp_label, "--°C");
        lv_obj_set_style_bg_color(s_temp_bar, EV_COLOR_SUBTEXT, LV_PART_INDICATOR);
    } else {
        const int temperature = rounded(state->temperature_c);
        lv_bar_set_value(s_temp_bar, temperature, LV_ANIM_ON);
        lv_label_set_text_fmt(s_temp_label, "%d°C", temperature);
        lv_obj_set_style_bg_color(s_temp_bar,
            temperature > 45 ? EV_COLOR_DANGER : (temperature > 38 ? EV_COLOR_WARN : EV_COLOR_ACCENT),
            LV_PART_INDICATOR);
    }
}

lv_obj_t * ev_screen_dashboard_create(lv_obj_t * parent)
{
    lv_obj_t * root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, EV_DISP_W, EV_CONTENT_H);
    lv_obj_set_style_pad_all(root, 6, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * main_row = lv_obj_create(root);
    lv_obj_remove_style_all(main_row);
    lv_obj_set_size(main_row, 308, 174);
    lv_obj_set_flex_flow(main_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(main_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(main_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(main_row, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t * arc_wrap = lv_obj_create(main_row);
    lv_obj_remove_style_all(arc_wrap);
    lv_obj_set_size(arc_wrap, 140, 174);
    lv_obj_remove_flag(arc_wrap, LV_OBJ_FLAG_SCROLLABLE);

    s_soc_arc = lv_arc_create(arc_wrap);
    lv_obj_set_size(s_soc_arc, 116, 116);
    lv_arc_set_rotation(s_soc_arc, 270);
    lv_arc_set_bg_angles(s_soc_arc, 0, 360);
    lv_arc_set_range(s_soc_arc, 0, 100);
    lv_obj_set_style_arc_width(s_soc_arc, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_soc_arc, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_soc_arc, EV_COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_soc_arc, EV_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_remove_style(s_soc_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_soc_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_soc_arc, LV_ALIGN_TOP_MID, 0, 2);

    s_soc_label = lv_label_create(s_soc_arc);
    lv_obj_set_style_text_font(s_soc_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_soc_label, EV_COLOR_TEXT, 0);
    lv_obj_center(s_soc_label);

    /* Estimator readout in the clear band between the arc and the range line. */
    s_estimator_label = lv_label_create(arc_wrap);
    lv_obj_set_style_text_font(s_estimator_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_estimator_label, EV_COLOR_SUBTEXT, 0);
    lv_label_set_text(s_estimator_label, "EKF init");
    lv_obj_align(s_estimator_label, LV_ALIGN_TOP_MID, 0, 122);

    s_range_label = lv_label_create(arc_wrap);
    lv_obj_set_style_text_color(s_range_label, EV_COLOR_SUBTEXT, 0);
    lv_obj_set_style_text_font(s_range_label, &lv_font_montserrat_12, 0);
    lv_obj_align(s_range_label, LV_ALIGN_BOTTOM_MID, 0, -14);

    s_health_label = lv_label_create(arc_wrap);
    lv_obj_set_style_text_font(s_health_label, &lv_font_montserrat_12, 0);
    lv_obj_align(s_health_label, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_t * stats = lv_obj_create(main_row);
    lv_obj_remove_style_all(stats);
    lv_obj_set_size(stats, 155, 174);
    lv_obj_set_flex_flow(stats, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(stats, 4, 0);
    lv_obj_set_flex_align(stats, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(stats, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * cards[3];
    const char * card_titles[3] = {"Current", "Voltage", "Temp"};
    const int card_heights[3] = {48, 48, 52};
    for (int i = 0; i < 3; i++) {
        cards[i] = lv_obj_create(stats);
        lv_obj_set_size(cards[i], 150, card_heights[i]);
        lv_obj_set_style_bg_color(cards[i], EV_COLOR_CARD, 0);
        lv_obj_set_style_radius(cards[i], 8, 0);
        lv_obj_set_style_border_width(cards[i], 0, 0);
        lv_obj_set_style_pad_all(cards[i], 6, 0);
        lv_obj_remove_flag(cards[i], LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * title = lv_label_create(cards[i]);
        lv_label_set_text(title, card_titles[i]);
        lv_obj_set_style_text_color(title, EV_COLOR_SUBTEXT, 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
    }

    s_current_arrow = lv_label_create(cards[0]);
    lv_obj_align(s_current_arrow, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    s_current_label = lv_label_create(cards[0]);
    lv_obj_set_style_text_color(s_current_label, EV_COLOR_TEXT, 0);
    lv_obj_align(s_current_label, LV_ALIGN_BOTTOM_LEFT, 18, 0);

    s_voltage_label = lv_label_create(cards[1]);
    lv_obj_set_style_text_color(s_voltage_label, EV_COLOR_TEXT, 0);
    lv_obj_align(s_voltage_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    s_temp_label = lv_label_create(cards[2]);
    lv_obj_set_style_text_color(s_temp_label, EV_COLOR_TEXT, 0);
    lv_obj_align(s_temp_label, LV_ALIGN_TOP_RIGHT, 0, 0);

    s_temp_bar = lv_bar_create(cards[2]);
    lv_obj_set_size(s_temp_bar, 136, 8);
    lv_bar_set_range(s_temp_bar, 15, 60);
    lv_obj_set_style_bg_color(s_temp_bar, lv_color_hex(0x2a2f35), LV_PART_MAIN);
    lv_obj_set_style_radius(s_temp_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(s_temp_bar, 4, LV_PART_INDICATOR);
    lv_obj_align(s_temp_bar, LV_ALIGN_BOTTOM_MID, 0, 0);

    return root;
}
