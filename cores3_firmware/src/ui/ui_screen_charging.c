#include "ui.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Full-width route map: roughly 1.6x the drawing area of the earlier
   side-by-side layout, which matters a lot at 320x240. */
#define MAP_W 312
#define MAP_H 96
#define MAP_MARGIN 12
#define SCALE_TARGET_PX 90

static lv_obj_t * s_title_label;
static lv_obj_t * s_action_label;
static lv_obj_t * s_mode_button;
static lv_obj_t * s_mode_label;
static lv_obj_t * s_location_label;
static lv_obj_t * s_message_label;
static lv_obj_t * s_list;

/* Route detail: large next-maneuver band above the decoded route shape. */
static lv_obj_t * s_route_panel;
static lv_obj_t * s_hero_icon;
static lv_obj_t * s_hero_distance;
static lv_obj_t * s_hero_instruction;
static lv_obj_t * s_map_panel;
static lv_obj_t * s_route_line;
static lv_obj_t * s_marker_origin;
static lv_obj_t * s_marker_destination;
static lv_obj_t * s_station_markers[EV_NAV_MAX_STATIONS];
static lv_obj_t * s_scale_bar;
static lv_obj_t * s_scale_label;
static lv_obj_t * s_map_hint;

static lv_point_precise_t s_line_points[EV_NAV_MAX_ROUTE_POINTS];
static uint8_t s_line_point_count;

static bool s_has_battery_state;
static bool s_route_view;
static bool s_steps_view;
static bool s_pending_rebuild;
static bool s_route_request_pending;
static bool s_refresh_request_pending;
static uint8_t s_requested_station;

static ev_battery_state_t s_battery_state;
static ev_navigation_snapshot_t s_navigation;
static int s_last_range_km = -1000;
static uint32_t s_last_station_revision = UINT32_MAX;
static uint32_t s_last_route_revision = UINT32_MAX;

/* Human distance: metres under 1 km, one-decimal km above. Writes into buf. */
static void format_distance(char * buf, size_t n, float km)
{
    if (km < 0.0f) {
        snprintf(buf, n, "--");
    } else if (km < 1.0f) {
        snprintf(buf, n, "%d m", (int)(km * 1000.0f + 0.5f));
    } else {
        snprintf(buf, n, "%.1f km", (double)km);
    }
}

static const char * reachability(float distance_km)
{
    if (!s_has_battery_state || !s_battery_state.sensor_data_valid || distance_km < 0.0f) {
        return "Range unknown";
    }
    if (distance_km + 5.0f <= s_battery_state.conservative_range_km) return "Reachable";
    if (distance_km <= s_battery_state.nominal_range_km) return "Marginal";
    return "Out of range";
}

static lv_color_t station_status_color(ev_station_status_t status)
{
    switch (status) {
        case EV_STATION_ONLINE: return EV_COLOR_ACCENT;
        case EV_STATION_CONNECTING:
        case EV_STATION_FETCHING:
        case EV_STATION_CACHED: return EV_COLOR_WARN;
        case EV_STATION_OFFLINE:
        case EV_STATION_ERROR: return EV_COLOR_DANGER;
        case EV_STATION_CONFIG_REQUIRED:
        default: return EV_COLOR_SUBTEXT;
    }
}

/* openrouteservice step types: 0/1/2 turns, 6 continue, 10 arrive, 12/13 keep. */
static const char * maneuver_icon(uint8_t type)
{
    switch (type) {
        case 0U: case 2U: case 4U: case 12U: return LV_SYMBOL_LEFT;
        case 10U: return LV_SYMBOL_OK;
        case 6U: return LV_SYMBOL_UP;
        default: return LV_SYMBOL_RIGHT;
    }
}

static void set_location_text(void)
{
    const ev_location_snapshot_t * location = &s_navigation.location;
    lv_color_t colour = EV_COLOR_TEXT;

    if (location->source == EV_LOCATION_GPS_FIX) {
        lv_label_set_text_fmt(s_location_label, LV_SYMBOL_GPS " %s | %u sat | HDOP %.1f",
                              ev_location_source_name(location->source),
                              location->satellites, (double)location->hdop);
        colour = EV_COLOR_ACCENT;
    } else if (location->source == EV_LOCATION_LAST_FIX) {
        const unsigned long age_s = location->fix_age_ms == UINT32_MAX
            ? 0UL : (unsigned long)(location->fix_age_ms / 1000U);
        lv_label_set_text_fmt(s_location_label, LV_SYMBOL_GPS " %s | %lus old | %u sat",
                              ev_location_source_name(location->source),
                              age_s, location->satellites);
        colour = EV_COLOR_WARN;
    } else if (location->link == EV_GPS_LINK_NO_DATA) {
        /* No NMEA at all: the receiver is not wired/powered, or truly nothing. */
        lv_label_set_text(s_location_label,
                          LV_SYMBOL_WARNING " No GPS module - using set location");
        colour = EV_COLOR_DANGER;
    } else if (location->source == EV_LOCATION_SAVED_FIX) {
        /* No live fix, but we restored the last good position from flash. */
        if (location->link == EV_GPS_LINK_ACQUIRING) {
            lv_label_set_text_fmt(s_location_label,
                                  LV_SYMBOL_GPS " Saved location - searching sky (%u sat)",
                                  location->satellites);
        } else {
            lv_label_set_text(s_location_label,
                              LV_SYMBOL_GPS " Saved location - no GPS signal");
        }
        colour = EV_COLOR_WARN;
    } else if (location->link == EV_GPS_LINK_ACQUIRING) {
        /* Data flowing, no fix: indoors / under cover. This is the case the
         * user asked to warn about. */
        lv_label_set_text_fmt(s_location_label,
                              LV_SYMBOL_WARNING " Searching sky (%u sat) - move to open area",
                              location->satellites);
        colour = EV_COLOR_WARN;
    } else {
        lv_label_set_text_fmt(s_location_label, "%s | %.4f, %.4f",
                              ev_location_source_name(location->source),
                              location->latitude, location->longitude);
        colour = EV_COLOR_SUBTEXT;
    }
    lv_obj_set_style_text_color(s_location_label, colour, 0);
}

static void add_empty_message(const char * text)
{
    lv_obj_t * label = lv_list_add_text(s_list, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(label, EV_COLOR_SUBTEXT, 0);
}

static void station_clicked_cb(lv_event_t * event)
{
    const uint8_t index = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    if (index >= s_navigation.station_count) return;

    s_requested_station = index;
    s_route_request_pending = true;
    s_route_view = true;
    s_steps_view = false;
    s_navigation.selected_station = (int8_t)index;
    s_navigation.route_status = EV_ROUTE_REQUESTED;
    s_navigation.route_point_count = 0U;
    snprintf(s_navigation.message, sizeof(s_navigation.message), "Route request queued");
    s_last_route_revision = UINT32_MAX;
    s_pending_rebuild = true;
}

static void action_clicked_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    if (s_route_view) {
        s_route_view = false;
        s_steps_view = false;
        s_last_station_revision = UINT32_MAX;
    } else {
        s_refresh_request_pending = true;
        snprintf(s_navigation.message, sizeof(s_navigation.message), "Refresh requested");
    }
    s_pending_rebuild = true;
}

static void mode_clicked_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    if (!s_route_view) return;
    s_steps_view = !s_steps_view;
    s_pending_rebuild = true;
}

static void populate_station_list(void)
{
    lv_obj_clean(s_list);
    if (s_navigation.station_count == 0U) {
        add_empty_message(s_navigation.message[0] != '\0'
            ? s_navigation.message : "No charging stations loaded");
        return;
    }

    for (uint8_t i = 0U; i < s_navigation.station_count; ++i) {
        const ev_charge_station_t * station = &s_navigation.stations[i];
        const float distance = station->route_distance_km >= 0.0f
            ? station->route_distance_km : station->straight_distance_km;
        const char * status = reachability(distance);
        const bool routed = station->route_distance_km >= 0.0f;

        /* Build the row manually. Using lv_list_add_button's built-in text plus
         * a column flow was hiding the name, so we add both lines ourselves:
         * NAME on top (headline), distance/status below. */
        lv_obj_t * button = lv_list_add_button(s_list, NULL, NULL);
        lv_obj_set_style_pad_ver(button, 5, 0);
        lv_obj_set_flex_flow(button, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(button, 2, 0);
        lv_obj_add_event_cb(button, station_clicked_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);

        lv_obj_t * name = lv_label_create(button);
        lv_label_set_text_fmt(name, LV_SYMBOL_CHARGE " %s", station->name);
        lv_obj_set_width(name, 280);
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_color(name, EV_COLOR_TEXT, 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_14, 0);

        char dist[16];
        /* "~" marks a straight-line estimate before a driving route exists. */
        format_distance(dist, sizeof(dist),
                        routed ? station->route_distance_km : station->straight_distance_km);
        lv_obj_t * sub = lv_label_create(button);
        lv_label_set_text_fmt(sub, "%s%s  |  %s", routed ? "" : "~", dist, status);
        lv_color_t status_colour = strcmp(status, "Reachable") == 0 ? EV_COLOR_ACCENT :
            (strcmp(status, "Marginal") == 0 ? EV_COLOR_WARN : EV_COLOR_DANGER);
        lv_obj_set_style_text_color(sub, status_colour, 0);
        lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
    }
}

static void populate_step_list(void)
{
    lv_obj_clean(s_list);
    if (s_navigation.route_status != EV_ROUTE_READY || s_navigation.maneuver_count == 0U) {
        add_empty_message(s_navigation.message[0] != '\0'
            ? s_navigation.message : "No route instructions yet");
        return;
    }

    for (uint8_t i = 0U; i < s_navigation.maneuver_count; ++i) {
        const ev_route_maneuver_t * maneuver = &s_navigation.maneuvers[i];
        lv_obj_t * row = lv_list_add_button(s_list, NULL, NULL);
        lv_obj_set_style_pad_ver(row, 5, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(row, 2, 0);

        lv_obj_t * instr = lv_label_create(row);
        lv_label_set_text_fmt(instr, "%s %s", maneuver_icon(maneuver->type),
                              maneuver->instruction);
        lv_obj_set_width(instr, 280);
        lv_label_set_long_mode(instr, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_color(instr, EV_COLOR_TEXT, 0);
        lv_obj_set_style_text_font(instr, &lv_font_montserrat_14, 0);

        char dist[16];
        format_distance(dist, sizeof(dist), maneuver->distance_km);
        lv_obj_t * sub = lv_label_create(row);
        if (maneuver->road_name[0] != '\0') {
            lv_label_set_text_fmt(sub, "%s  |  %s", dist, maneuver->road_name);
        } else {
            lv_label_set_text(sub, dist);
        }
        lv_obj_set_style_text_color(sub, EV_COLOR_SUBTEXT, 0);
        lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
    }
}

static void hide_map_overlays(void)
{
    lv_obj_add_flag(s_route_line, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_marker_origin, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_marker_destination, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_scale_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_scale_label, LV_OBJ_FLAG_HIDDEN);
    for (uint8_t i = 0U; i < EV_NAV_MAX_STATIONS; ++i) {
        lv_obj_add_flag(s_station_markers[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* Pick a round scale-bar length that renders near SCALE_TARGET_PX. */
static void update_scale_bar(float km_per_px)
{
    static const float candidates[] = {0.2f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 20.0f, 50.0f, 100.0f};
    float chosen_km = candidates[0];
    int chosen_px = 0;

    for (size_t i = 0U; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        const int px = (int)(candidates[i] / km_per_px + 0.5f);
        if (px <= SCALE_TARGET_PX && px >= 20) {
            chosen_km = candidates[i];
            chosen_px = px;
        }
    }
    if (chosen_px == 0) {
        lv_obj_add_flag(s_scale_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_scale_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_set_size(s_scale_bar, chosen_px, 2);
    lv_obj_set_pos(s_scale_bar, 6, MAP_H - 8);
    lv_obj_remove_flag(s_scale_bar, LV_OBJ_FLAG_HIDDEN);

    if (chosen_km < 1.0f) {
        lv_label_set_text_fmt(s_scale_label, "%d m", (int)(chosen_km * 1000.0f + 0.5f));
    } else {
        lv_label_set_text_fmt(s_scale_label, "%d km", (int)(chosen_km + 0.5f));
    }
    lv_obj_set_pos(s_scale_label, chosen_px + 10, MAP_H - 16);
    lv_obj_remove_flag(s_scale_label, LV_OBJ_FLAG_HIDDEN);
}

/* Equirectangular projection of the decoded route into the map panel, with
   longitude compressed by cos(latitude) so the shape keeps its real aspect.
   Other nearby stations are overlaid when they fall inside the route's view. */
static void update_route_map(void)
{
    const uint8_t count = s_navigation.route_point_count;
    if (count < 2U) {
        hide_map_overlays();
        lv_obj_remove_flag(s_map_hint, LV_OBJ_FLAG_HIDDEN);
        const char * hint;
        switch (s_navigation.route_status) {
            case EV_ROUTE_REQUESTED:
            case EV_ROUTE_FETCHING: hint = LV_SYMBOL_REFRESH " Calculating route..."; break;
            case EV_ROUTE_ERROR:    hint = "Route unavailable"; break;
            case EV_ROUTE_READY:    hint = "Route shape unavailable"; break;
            default:                hint = "Select a charger"; break;
        }
        lv_label_set_text(s_map_hint, hint);
        return;
    }

    lv_obj_add_flag(s_map_hint, LV_OBJ_FLAG_HIDDEN);

    float min_lat = s_navigation.route_points[0].latitude;
    float max_lat = min_lat;
    float min_lon = s_navigation.route_points[0].longitude;
    float max_lon = min_lon;
    for (uint8_t i = 1U; i < count; ++i) {
        const float lat = s_navigation.route_points[i].latitude;
        const float lon = s_navigation.route_points[i].longitude;
        if (lat < min_lat) min_lat = lat;
        if (lat > max_lat) max_lat = lat;
        if (lon < min_lon) min_lon = lon;
        if (lon > max_lon) max_lon = lon;
    }

    const float mid_lat = (min_lat + max_lat) * 0.5f;
    const float mid_lon = (min_lon + max_lon) * 0.5f;
    const float lon_scale = cosf(mid_lat * 0.017453292f);
    float span_x = (max_lon - min_lon) * lon_scale;
    float span_y = (max_lat - min_lat);
    if (span_x < 1e-6f) span_x = 1e-6f;
    if (span_y < 1e-6f) span_y = 1e-6f;

    const float usable_w = (float)(MAP_W - 2 * MAP_MARGIN);
    const float usable_h = (float)(MAP_H - 2 * MAP_MARGIN);
    const float scale_x = usable_w / span_x;
    const float scale_y = usable_h / span_y;
    const float scale = scale_x < scale_y ? scale_x : scale_y;
    const float center_x = (float)MAP_W * 0.5f;
    const float center_y = (float)MAP_H * 0.5f;

    for (uint8_t i = 0U; i < count; ++i) {
        const float dx = (s_navigation.route_points[i].longitude - mid_lon) * lon_scale * scale;
        const float dy = (s_navigation.route_points[i].latitude - mid_lat) * scale;
        s_line_points[i].x = (lv_value_precise_t)(center_x + dx);
        s_line_points[i].y = (lv_value_precise_t)(center_y - dy);
    }
    s_line_point_count = count;

    lv_line_set_points(s_route_line, s_line_points, s_line_point_count);
    lv_obj_remove_flag(s_route_line, LV_OBJ_FLAG_HIDDEN);

    lv_obj_remove_flag(s_marker_origin, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_marker_destination, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_marker_origin,
                   (int32_t)s_line_points[0].x - 5, (int32_t)s_line_points[0].y - 5);
    lv_obj_set_pos(s_marker_destination,
                   (int32_t)s_line_points[count - 1U].x - 5,
                   (int32_t)s_line_points[count - 1U].y - 5);

    /* Context: other chargers that happen to lie within the routed view. */
    for (uint8_t i = 0U; i < EV_NAV_MAX_STATIONS; ++i) {
        lv_obj_t * marker = s_station_markers[i];
        if (i >= s_navigation.station_count || (int8_t)i == s_navigation.selected_station) {
            lv_obj_add_flag(marker, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const float dx = ((float)s_navigation.stations[i].longitude - mid_lon) * lon_scale * scale;
        const float dy = ((float)s_navigation.stations[i].latitude - mid_lat) * scale;
        const int x = (int)(center_x + dx);
        const int y = (int)(center_y - dy);
        if (x < 2 || x > MAP_W - 2 || y < 2 || y > MAP_H - 2) {
            lv_obj_add_flag(marker, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_set_pos(marker, x - 3, y - 3);
        lv_obj_remove_flag(marker, LV_OBJ_FLAG_HIDDEN);
    }

    /* 1 degree of latitude is ~110.574 km. */
    update_scale_bar(110.574f / scale);
}

static void update_route_hero(void)
{
    const bool ready = s_navigation.route_status == EV_ROUTE_READY &&
        s_navigation.maneuver_count > 0U;
    if (!ready) {
        lv_label_set_text(s_hero_icon, LV_SYMBOL_REFRESH);
        lv_label_set_text(s_hero_distance, "--");
        lv_label_set_text(s_hero_instruction, s_navigation.message[0] != '\0'
                          ? s_navigation.message : "Calculating route");
        return;
    }

    const uint8_t index = s_navigation.current_maneuver < s_navigation.maneuver_count
        ? s_navigation.current_maneuver : 0U;
    const ev_route_maneuver_t * maneuver = &s_navigation.maneuvers[index];

    lv_label_set_text(s_hero_icon, maneuver_icon(maneuver->type));
    if (maneuver->distance_km < 1.0f) {
        lv_label_set_text_fmt(s_hero_distance, "%d m",
                              (int)(maneuver->distance_km * 1000.0f + 0.5f));
    } else {
        lv_label_set_text_fmt(s_hero_distance, "%.1f km", (double)maneuver->distance_km);
    }

    if (maneuver->road_name[0] != '\0') {
        lv_label_set_text_fmt(s_hero_instruction, "%s\n%s",
                              maneuver->instruction, maneuver->road_name);
    } else {
        lv_label_set_text(s_hero_instruction, maneuver->instruction);
    }
}

static void render_station_view(bool rebuild_list)
{
    lv_obj_add_flag(s_route_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_location_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_mode_button, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(s_title_label, "Nearby chargers");
    lv_label_set_text(s_action_label, LV_SYMBOL_REFRESH);
    set_location_text();

    if (s_navigation.station_age_s == EV_NAV_AGE_UNKNOWN) {
        lv_label_set_text_fmt(s_message_label, "%s | %s",
                              ev_station_status_name(s_navigation.station_status),
                              s_navigation.message);
    } else if (s_navigation.station_age_s < 60U) {
        lv_label_set_text_fmt(s_message_label, "%s | %lus ago",
                              ev_station_status_name(s_navigation.station_status),
                              (unsigned long)s_navigation.station_age_s);
    } else {
        lv_label_set_text_fmt(s_message_label, "%s | %lum ago",
                              ev_station_status_name(s_navigation.station_status),
                              (unsigned long)(s_navigation.station_age_s / 60U));
    }
    lv_obj_set_style_text_color(s_message_label,
                                station_status_color(s_navigation.station_status), 0);
    if (rebuild_list) populate_station_list();
}

static void render_route_view(bool rebuild_list)
{
    const ev_charge_station_t * station = NULL;
    if (s_navigation.selected_station >= 0 &&
        s_navigation.selected_station < (int8_t)s_navigation.station_count) {
        station = &s_navigation.stations[s_navigation.selected_station];
    }

    lv_label_set_text(s_title_label, station != NULL ? station->name : "Turn-by-turn");
    lv_label_set_text(s_action_label, LV_SYMBOL_LEFT);
    lv_obj_remove_flag(s_mode_button, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_mode_label, s_steps_view ? LV_SYMBOL_IMAGE : LV_SYMBOL_LIST);

    /* The map needs the height, so fold the location source into the summary. */
    lv_obj_add_flag(s_location_label, LV_OBJ_FLAG_HIDDEN);

    if (s_navigation.route_status == EV_ROUTE_READY) {
        const char * status = reachability(s_navigation.route_distance_km);
        lv_label_set_text_fmt(s_message_label, "%.1f km | ETA %lu min | %s | %s",
                              (double)s_navigation.route_distance_km,
                              (unsigned long)((s_navigation.route_duration_s + 59U) / 60U),
                              status,
                              ev_location_source_name(s_navigation.location.source));
        lv_obj_set_style_text_color(s_message_label,
                                    strcmp(status, "Reachable") == 0
                                        ? EV_COLOR_ACCENT : EV_COLOR_WARN, 0);
    } else {
        lv_label_set_text_fmt(s_message_label, "%s | %s",
                              ev_route_status_name(s_navigation.route_status),
                              s_navigation.message);
        lv_obj_set_style_text_color(s_message_label,
                                    s_navigation.route_status == EV_ROUTE_ERROR
                                        ? EV_COLOR_DANGER : EV_COLOR_WARN, 0);
    }

    if (s_steps_view) {
        lv_obj_add_flag(s_route_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_list, LV_OBJ_FLAG_HIDDEN);
        if (rebuild_list) populate_step_list();
        return;
    }

    lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_route_panel, LV_OBJ_FLAG_HIDDEN);
    update_route_hero();
    update_route_map();
}

static void render_current_view(bool rebuild_list)
{
    if (s_route_view) render_route_view(rebuild_list);
    else render_station_view(rebuild_list);
}

void ev_screen_charging_update(const ev_battery_state_t * state)
{
    if (state == NULL) return;
    s_battery_state = *state;
    s_has_battery_state = true;

    const int range_km = (int)(state->conservative_range_km + 0.5f);
    if (range_km != s_last_range_km) {
        s_last_range_km = range_km;
        s_pending_rebuild = true;
    }
}

void ev_screen_charging_navigation_update(const ev_navigation_snapshot_t * navigation)
{
    if (navigation != NULL) {
        const int8_t selected = s_navigation.selected_station;
        const bool keep_local_selection =
            s_route_request_pending && navigation->route_revision == s_last_route_revision;
        s_navigation = *navigation;
        if (keep_local_selection) s_navigation.selected_station = selected;

        if (s_route_view) {
            if (navigation->route_revision != s_last_route_revision) {
                s_last_route_revision = navigation->route_revision;
                s_pending_rebuild = true;
            }
        } else if (navigation->station_revision != s_last_station_revision) {
            s_last_station_revision = navigation->station_revision;
            s_pending_rebuild = true;
        }
    }

    const bool rebuild = s_pending_rebuild;
    s_pending_rebuild = false;
    render_current_view(rebuild);
}

bool ev_screen_charging_take_route_request(uint8_t * station_index)
{
    if (!s_route_request_pending || station_index == NULL) return false;
    *station_index = s_requested_station;
    s_route_request_pending = false;
    return true;
}

bool ev_screen_charging_take_refresh_request(void)
{
    if (!s_refresh_request_pending) return false;
    s_refresh_request_pending = false;
    return true;
}

static lv_obj_t * create_header_button(lv_obj_t * parent, lv_event_cb_t handler,
                                      lv_obj_t ** out_label, const char * symbol)
{
    /* 40x26 with a generous hit area: the two header buttons were 30x20 and
     * only 4 px apart, which caused frequent mis-touches between them. */
    lv_obj_t * button = lv_button_create(parent);
    lv_obj_set_size(button, 40, 26);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_bg_color(button, EV_COLOR_CARD, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    /* Extra invisible touch margin so adjacent taps don't fall between them. */
    lv_obj_set_ext_click_area(button, 4);
    lv_obj_add_event_cb(button, handler, LV_EVENT_CLICKED, NULL);

    lv_obj_t * label = lv_label_create(button);
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_color(label, EV_COLOR_ACCENT, 0);
    lv_obj_center(label);
    *out_label = label;
    return button;
}

static lv_obj_t * create_dot(lv_obj_t * parent, int size, lv_color_t color)
{
    lv_obj_t * dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, size, size);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, color, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
    return dot;
}

static void build_route_panel(lv_obj_t * parent)
{
    s_route_panel = lv_obj_create(parent);
    lv_obj_remove_style_all(s_route_panel);
    lv_obj_set_size(s_route_panel, 312, 138);
    lv_obj_set_flex_flow(s_route_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_route_panel, 2, 0);
    lv_obj_remove_flag(s_route_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_route_panel, LV_OBJ_FLAG_HIDDEN);

    /* Next-maneuver band: large arrow and distance, instruction alongside. */
    lv_obj_t * hero = lv_obj_create(s_route_panel);
    lv_obj_remove_style_all(hero);
    lv_obj_set_size(hero, 312, 42);
    lv_obj_set_style_bg_color(hero, EV_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(hero, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(hero, 8, 0);
    lv_obj_set_style_pad_hor(hero, 8, 0);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hero, 8, 0);
    lv_obj_remove_flag(hero, LV_OBJ_FLAG_SCROLLABLE);

    s_hero_icon = lv_label_create(hero);
    lv_label_set_text(s_hero_icon, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(s_hero_icon, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_hero_icon, EV_COLOR_ACCENT, 0);

    s_hero_distance = lv_label_create(hero);
    lv_label_set_text(s_hero_distance, "--");
    lv_obj_set_style_text_font(s_hero_distance, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_hero_distance, EV_COLOR_TEXT, 0);

    s_hero_instruction = lv_label_create(hero);
    lv_label_set_text(s_hero_instruction, "Calculating route");
    lv_obj_set_width(s_hero_instruction, 150);
    lv_label_set_long_mode(s_hero_instruction, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(s_hero_instruction, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_hero_instruction, EV_COLOR_SUBTEXT, 0);

    s_map_panel = lv_obj_create(s_route_panel);
    lv_obj_remove_style_all(s_map_panel);
    lv_obj_set_size(s_map_panel, MAP_W, MAP_H);
    lv_obj_set_style_bg_color(s_map_panel, lv_color_hex(0x151b21), 0);
    lv_obj_set_style_bg_opa(s_map_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_map_panel, 8, 0);
    lv_obj_set_style_pad_all(s_map_panel, 0, 0);
    lv_obj_remove_flag(s_map_panel, LV_OBJ_FLAG_SCROLLABLE);

    s_route_line = lv_line_create(s_map_panel);
    lv_obj_set_style_line_width(s_route_line, 4, 0);
    lv_obj_set_style_line_color(s_route_line, EV_COLOR_ACCENT, 0);
    lv_obj_set_style_line_rounded(s_route_line, true, 0);
    lv_obj_add_flag(s_route_line, LV_OBJ_FLAG_HIDDEN);

    for (uint8_t i = 0U; i < EV_NAV_MAX_STATIONS; ++i) {
        s_station_markers[i] = create_dot(s_map_panel, 6, EV_COLOR_SUBTEXT);
    }
    s_marker_origin = create_dot(s_map_panel, 10, EV_COLOR_TEXT);
    s_marker_destination = create_dot(s_map_panel, 10, EV_COLOR_DANGER);

    s_scale_bar = lv_obj_create(s_map_panel);
    lv_obj_remove_style_all(s_scale_bar);
    lv_obj_set_size(s_scale_bar, 40, 2);
    lv_obj_set_style_bg_color(s_scale_bar, EV_COLOR_SUBTEXT, 0);
    lv_obj_set_style_bg_opa(s_scale_bar, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_scale_bar, LV_OBJ_FLAG_HIDDEN);

    s_scale_label = lv_label_create(s_map_panel);
    lv_label_set_text(s_scale_label, "");
    lv_obj_set_style_text_font(s_scale_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_scale_label, EV_COLOR_SUBTEXT, 0);
    lv_obj_add_flag(s_scale_label, LV_OBJ_FLAG_HIDDEN);

    s_map_hint = lv_label_create(s_map_panel);
    lv_label_set_text(s_map_hint, "Waiting for route");
    lv_obj_set_style_text_font(s_map_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_map_hint, EV_COLOR_SUBTEXT, 0);
    lv_obj_center(s_map_hint);
}

lv_obj_t * ev_screen_charging_create(lv_obj_t * parent)
{
    ev_navigation_state_init(&s_navigation);
    s_navigation.location.has_position = true;

    lv_obj_t * root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, EV_DISP_W, EV_CONTENT_H);
    lv_obj_set_style_pad_all(root, 4, 0);
    lv_obj_set_style_pad_row(root, 1, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * header = lv_obj_create(root);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, 312, 28);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header, 8, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    s_title_label = lv_label_create(header);
    lv_label_set_text(s_title_label, "Nearby chargers");
    lv_obj_set_flex_grow(s_title_label, 1);  /* takes remaining width */
    lv_label_set_long_mode(s_title_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_color(s_title_label, EV_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_title_label, &lv_font_montserrat_14, 0);

    /* Spacer forces the two buttons to opposite behaviour: title grows to fill,
     * then the buttons sit at the right with a clear 8 px gap between them. */
    s_mode_button = create_header_button(header, mode_clicked_cb, &s_mode_label,
                                        LV_SYMBOL_LIST);
    lv_obj_add_flag(s_mode_button, LV_OBJ_FLAG_HIDDEN);
    create_header_button(header, action_clicked_cb, &s_action_label, LV_SYMBOL_REFRESH);

    s_location_label = lv_label_create(root);
    lv_obj_set_width(s_location_label, 312);
    lv_obj_set_style_text_font(s_location_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_location_label, EV_COLOR_TEXT, 0);

    s_message_label = lv_label_create(root);
    lv_obj_set_width(s_message_label, 312);
    lv_label_set_long_mode(s_message_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(s_message_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_message_label, EV_COLOR_SUBTEXT, 0);

    s_list = lv_list_create(root);
    lv_obj_set_size(s_list, 312, 116);
    lv_obj_set_style_bg_color(s_list, EV_COLOR_CARD, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_radius(s_list, 8, 0);
    lv_obj_set_style_pad_all(s_list, 4, 0);

    build_route_panel(root);

    render_current_view(true);
    return root;
}
