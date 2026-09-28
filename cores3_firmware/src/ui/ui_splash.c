#include "ui.h"

/* Boot splash implemented as a full-screen opaque OVERLAY on top of the main UI
 * (not a separate LVGL screen). Dismissing is just deleting the overlay, which
 * reveals the already-built UI underneath. This avoids lv_screen_load handoff
 * issues that left the device stuck on the splash. */

static lv_obj_t * s_overlay;
static lv_obj_t * s_status_label;
static lv_obj_t * s_bar;

lv_obj_t * ev_splash_create(void)
{
    /* Cover the active screen (the main UI has already been built on it). */
    lv_obj_t * parent = lv_screen_active();

    s_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, EV_DISP_W, EV_DISP_H);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, EV_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    /* Created last on the parent, so it is already the top-most child and
     * covers the UI beneath it. */

    lv_obj_t * ring = lv_arc_create(s_overlay);
    lv_obj_set_size(ring, 96, 96);
    lv_arc_set_rotation(ring, 270);
    lv_arc_set_bg_angles(ring, 0, 360);
    lv_arc_set_value(ring, 72);
    lv_obj_set_style_arc_width(ring, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ring, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ring, EV_COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ring, EV_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(ring, LV_ALIGN_CENTER, 0, -34);

    lv_obj_t * mark = lv_label_create(ring);
    lv_label_set_text(mark, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_font(mark, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(mark, EV_COLOR_ACCENT, 0);
    lv_obj_center(mark);

    lv_obj_t * title = lv_label_create(s_overlay);
    lv_label_set_text(title, "EV Battery Companion");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, EV_COLOR_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 30);

    lv_obj_t * subtitle = lv_label_create(s_overlay);
    lv_label_set_text(subtitle, "3S Li-ion NMC | GPS | Live chargers");
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(subtitle, EV_COLOR_SUBTEXT, 0);
    lv_obj_align(subtitle, LV_ALIGN_CENTER, 0, 48);

    s_bar = lv_bar_create(s_overlay);
    lv_obj_set_size(s_bar, 180, 4);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar, EV_COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, EV_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 2, LV_PART_INDICATOR);
    lv_obj_align(s_bar, LV_ALIGN_BOTTOM_MID, 0, -24);

    s_status_label = lv_label_create(s_overlay);
    lv_label_set_text(s_status_label, "Starting up");
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_status_label, EV_COLOR_SUBTEXT, 0);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_MID, 0, -8);

    return s_overlay;
}

void ev_splash_set_progress(int percent, const char * status)
{
    if (s_bar == NULL) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    lv_bar_set_value(s_bar, percent, LV_ANIM_ON);
    if (status != NULL && s_status_label != NULL) {
        lv_label_set_text(s_status_label, status);
    }
}

void ev_splash_finish(void)
{
    /* Delete the overlay -> the main UI underneath is revealed immediately.
     * A short fade-out keeps it smooth without any screen switching. */
    if (s_overlay == NULL) return;
    lv_obj_t * overlay = s_overlay;
    s_overlay = NULL;
    s_status_label = NULL;
    s_bar = NULL;

    lv_obj_fade_out(overlay, 250, 0);
    lv_obj_delete_delayed(overlay, 320);
}
