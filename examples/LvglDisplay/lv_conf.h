// Minimal LVGL 9 configuration for the standalone Xiaozhi display example.
// LVGL's internal defaults provide the remaining options.
#ifndef XIAOZHI_LV_CONF_H
#define XIAOZHI_LV_CONF_H

#define LV_CONF_H
#define LV_COLOR_DEPTH 16
#define LV_USE_LOG 0
#define LV_DRAW_SW_COMPLEX 0
#define LV_USE_FLEX 0
#define LV_USE_GRID 0

// This status screen only uses labels. Keep every other optional widget out of
// the example so it fits the ESP32-S3 default app partition without requiring
// a board-specific partition-table setting.
#define LV_USE_ANIMIMG 0
#define LV_USE_ARC 0
#define LV_USE_ARCLABEL 0
#define LV_USE_BAR 0
#define LV_USE_BUTTON 0
#define LV_USE_BUTTONMATRIX 0
#define LV_USE_CALENDAR 0
#define LV_USE_CANVAS 0
#define LV_USE_CHART 0
#define LV_USE_CHECKBOX 0
#define LV_USE_DROPDOWN 0
#define LV_USE_IMAGE 0
#define LV_USE_IMAGEBUTTON 0
#define LV_USE_KEYBOARD 0
#define LV_USE_LABEL 1
#define LV_LABEL_TEXT_SELECTION 0
#define LV_LABEL_LONG_TXT_HINT 0
#define LV_USE_LED 0
#define LV_USE_LINE 0
#define LV_USE_LIST 0
#define LV_USE_LOTTIE 0
#define LV_USE_MENU 0
#define LV_USE_MSGBOX 0
#define LV_USE_ROLLER 0
#define LV_USE_SCALE 0
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

// The example applies its own small set of styles. Disabling the bundled themes
// avoids pulling every themed widget into a default-size ESP32 application image.
#define LV_USE_THEME_DEFAULT 0
#define LV_USE_THEME_SIMPLE 0
#define LV_USE_THEME_MONO 0

#endif  // XIAOZHI_LV_CONF_H
