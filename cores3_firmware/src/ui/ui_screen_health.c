#include "ui.h"

static lv_obj_t * s_quality_arc;
static lv_obj_t * s_quality_label;
static lv_obj_t * s_health_status_label;
static lv_obj_t * s_cycle_status_label;
static lv_obj_t * s_resistance_status_label;
static lv_obj_t * s_voltage_chart;
static lv_chart_series_t * s_voltage_series;
static lv_obj_t * s_cell_label;

/* Series cell count of the live bench pack, for per-cell average display. */
#define HEALTH_PACK_CELLS_S 3

void ev_screen_health_update(const ev_battery_state_t * state)
{
    if (state == NULL) return;

    int quality_value = 100;
    const char * quality_text = "GOOD";
    lv_color_t quality_color = EV_COLOR_ACCENT;
    if (state->quality == EV_DATA_QUALITY_WARNING) {
        quality_value = 60;
        quality_text = "WARN";
        quality_color = EV_COLOR_WARN;
    } else if (state->quality == EV_DATA_QUALITY_FAULT) {
        quality_value = 10;
        quality_text = "FAULT";
        quality_color = EV_COLOR_DANGER;
    }

    lv_arc_set_value(s_quality_arc, quality_value);
    lv_obj_set_style_arc_color(s_quality_arc, quality_color, LV_PART_INDICATOR);
    lv_label_set_text(s_quality_label, quality_text);
    lv_obj_set_style_text_color(s_quality_label, quality_color, 0);

    /* Honest live health facts (no fabricated SoH/cycle "learning"). SoH needs a
     * full reference charge/discharge cycle to measure, which a short bench demo
     * has not performed, so we say so plainly and show the real measured pack
     * facts instead: pack voltage, per-cell average, and data quality. */
    if (state->sensor_data_valid) {
        const float cell_avg = state->voltage_v / (float)HEALTH_PACK_CELLS_S;
        lv_label_set_text_fmt(s_health_status_label, "Pack: %.2f V (%dS)",
                              (double)state->voltage_v, HEALTH_PACK_CELLS_S);
        lv_label_set_text_fmt(s_cycle_status_label, "Cell avg: %.2f V", (double)cell_avg);
        lv_label_set_text(s_resistance_status_label,
                          "SoH: needs full cycle to measure");
        if (s_cell_label != NULL) {
            lv_label_set_text_fmt(s_cell_label, "Per-cell\n%.2f V", (double)cell_avg);
        }
    } else {
        lv_label_set_text(s_health_status_label, "Pack: -- V");
        lv_label_set_text(s_cycle_status_label, "Cell avg: -- V");
        lv_label_set_text(s_resistance_status_label, "SoH: no sensor data");
    }

    if (state->sensor_data_valid) {
        /* Chart in tenths of a volt (x10) so the trend has usable resolution on
         * the integer chart; Y-range matches the live pack (see create()). */
        lv_chart_set_next_value(s_voltage_chart, s_voltage_series,
                                (int32_t)(state->voltage_v * 10.0f + 0.5f));
    }
}

lv_obj_t * ev_screen_health_create(lv_obj_t * parent)
{
    lv_obj_t * root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, EV_DISP_W, EV_CONTENT_H);
    lv_obj_set_style_pad_all(root, 6, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * top = lv_obj_create(root);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, 308, 78);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top, 8, 0);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(top, LV_ALIGN_TOP_LEFT, 0, 0);

    s_quality_arc = lv_arc_create(top);
    lv_obj_set_size(s_quality_arc, 72, 72);
    lv_arc_set_rotation(s_quality_arc, 270);
    lv_arc_set_bg_angles(s_quality_arc, 0, 360);
    lv_arc_set_range(s_quality_arc, 0, 100);
    lv_obj_set_style_arc_width(s_quality_arc, 7, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_quality_arc, 7, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_quality_arc, EV_COLOR_CARD, LV_PART_MAIN);
    lv_obj_remove_style(s_quality_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_quality_arc, LV_OBJ_FLAG_CLICKABLE);

    s_quality_label = lv_label_create(s_quality_arc);
    lv_obj_set_style_text_font(s_quality_label, &lv_font_montserrat_12, 0);
    lv_obj_center(s_quality_label);

    lv_obj_t * stats = lv_obj_create(top);
    lv_obj_remove_style_all(stats);
    lv_obj_set_size(stats, 220, 74);
    lv_obj_set_flex_flow(stats, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(stats, 3, 0);
    lv_obj_remove_flag(stats, LV_OBJ_FLAG_SCROLLABLE);

    s_health_status_label = lv_label_create(stats);
    s_cycle_status_label = lv_label_create(stats);
    s_resistance_status_label = lv_label_create(stats);
    lv_obj_t * status_labels[3] = {
        s_health_status_label, s_cycle_status_label, s_resistance_status_label
    };
    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_text_color(status_labels[i], EV_COLOR_SUBTEXT, 0);
        lv_obj_set_style_text_font(status_labels[i], &lv_font_montserrat_12, 0);
    }

    s_voltage_chart = lv_chart_create(root);
    lv_obj_set_size(s_voltage_chart, 180, 58);
    lv_chart_set_type(s_voltage_chart, LV_CHART_TYPE_LINE);
    /* Live 3S bench pack: ~9.0-13.0 V, charted in tenths of a volt (x10) for
     * resolution -> range 90..130. (The old 38-56 range was for a 13S pack and
     * pinned this pack to the chart floor.) */
    lv_chart_set_axis_range(s_voltage_chart, LV_CHART_AXIS_PRIMARY_Y, 90, 130);
    lv_chart_set_point_count(s_voltage_chart, 12);
    lv_obj_set_style_bg_color(s_voltage_chart, EV_COLOR_CARD, 0);
    lv_obj_set_style_border_width(s_voltage_chart, 0, 0);
    lv_obj_set_style_radius(s_voltage_chart, 6, 0);
    lv_obj_set_style_pad_all(s_voltage_chart, 4, 0);
    lv_obj_align(s_voltage_chart, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    s_voltage_series = lv_chart_add_series(s_voltage_chart, EV_COLOR_ACCENT, LV_CHART_AXIS_PRIMARY_Y);

    lv_obj_t * chart_title = lv_label_create(root);
    lv_label_set_text(chart_title, "Pack voltage trend");
    lv_obj_set_style_text_color(chart_title, EV_COLOR_SUBTEXT, 0);
    lv_obj_set_style_text_font(chart_title, &lv_font_montserrat_12, 0);
    lv_obj_align_to(chart_title, s_voltage_chart, LV_ALIGN_OUT_TOP_LEFT, 0, -2);

    lv_obj_t * cell_card = lv_obj_create(root);
    lv_obj_set_size(cell_card, 118, 58);
    lv_obj_set_style_bg_color(cell_card, EV_COLOR_CARD, 0);
    lv_obj_set_style_border_width(cell_card, 0, 0);
    lv_obj_set_style_radius(cell_card, 6, 0);
    lv_obj_align(cell_card, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    s_cell_label = lv_label_create(cell_card);
    lv_label_set_text(s_cell_label, "Per-cell\n-- V");
    lv_obj_set_style_text_color(s_cell_label, EV_COLOR_SUBTEXT, 0);
    lv_obj_set_style_text_font(s_cell_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(s_cell_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_cell_label);

    return root;
}
