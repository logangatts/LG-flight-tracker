/* LVGL 9 configuration for Flight Radar (ESP32-S3, 390x390 RGB565) */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_MEM_SIZE (64 * 1024U)

#define LV_COLOR_DEPTH 16

#define LV_DEF_REFR_PERIOD 33

#define LV_USE_OS LV_OS_NONE

#define LV_DRAW_SW_SUPPORT_RGB565 1
#define LV_DRAW_SW_SUPPORT_ARGB8888 1

#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1

/* Fonts */
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* Widgets we use */
#define LV_USE_LABEL 1
#define LV_USE_LINE 1
#define LV_USE_ARC 1
#define LV_USE_IMAGE 1
#define LV_USE_BUTTON 1
#define LV_USE_CANVAS 1

/* Others off to save flash */
#define LV_USE_ANIMIMG 0
#define LV_USE_CALENDAR 0
#define LV_USE_CHART 0
#define LV_USE_KEYBOARD 0
#define LV_USE_LIST 0
#define LV_USE_MENU 0
#define LV_USE_MSGBOX 0
#define LV_USE_ROLLER 0
#define LV_USE_SLIDER 0
#define LV_USE_SPAN 0
#define LV_USE_SPINBOX 0
#define LV_USE_SPINNER 0
#define LV_USE_SWITCH 0
#define LV_USE_TABLE 0
#define LV_USE_TABVIEW 0
#define LV_USE_TEXTAREA 0
#define LV_USE_TILEVIEW 0
#define LV_USE_WIN 0
#define LV_USE_DROPDOWN 0
#define LV_USE_CHECKBOX 0
#define LV_USE_BAR 1
#define LV_USE_BUTTONMATRIX 0
#define LV_USE_SCALE 0

#define LV_USE_THEME_DEFAULT 1
#define LV_USE_THEME_SIMPLE 0

#define LV_BUILD_EXAMPLES 0
#define LV_USE_DEMO_WIDGETS 0

#endif /* LV_CONF_H */
