#ifndef LV_CONF_H
#define LV_CONF_H

/* CoreS3-SE: 320x240 RGB565 display using M5Unified. */
#define LV_COLOR_DEPTH 16
#define LV_USE_OS LV_OS_NONE
#define LV_DEF_REFR_PERIOD 33

/* LVGL's built-in allocator; the UI creates all five screens at startup. */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
/* 80KiB is ample for these five text/list screens and leaves internal RAM
   for the Wi-Fi stack and TLS handshakes used by navigation. */
#define LV_MEM_SIZE (80U * 1024U)
#define LV_MEM_ADR 0
#define LV_MEM_POOL_EXPAND_SIZE 0

#define LV_USE_FLOAT 1
#define LV_TXT_ENC LV_TXT_ENC_UTF8

#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_USE_FLEX 1
#define LV_USE_GRID 0

/* Only the built-in fonts referenced by the EV UI. */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* Widgets used by the five screens. */
#define LV_USE_ARC 1
#define LV_USE_BAR 1
#define LV_USE_BUTTON 1
#define LV_USE_CHART 1
#define LV_USE_DROPDOWN 1
#define LV_USE_LABEL 1
#define LV_USE_LINE 1
#define LV_USE_LIST 1
#define LV_USE_SLIDER 1
#define LV_USE_SWITCH 1
#define LV_USE_TEXTAREA 1

/* No desktop drivers, demos, examples, or media decoders are required. */
#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0
#define LV_USE_SDL 0

#endif /* LV_CONF_H */
