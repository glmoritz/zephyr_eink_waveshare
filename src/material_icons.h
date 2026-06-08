#ifndef MATERIAL_ICONS_H_
#define MATERIAL_ICONS_H_

#include <lvgl.h>

extern const lv_font_t material_design_40;
extern const lv_font_t material_design_120;

/*
 * Material icon glyphs used by the local device UI.
 * Keep all icon mappings here so new glyphs can be added without scattering
 * raw UTF-8 byte sequences through the rendering code.
 */
#define ICON_BATTERY_0    "\xEE\x86\xA0"
#define ICON_BATTERY_1    "\xEE\x86\xA1"
#define ICON_BATTERY_2    "\xEE\x86\xA2"
#define ICON_BATTERY_3    "\xEE\x86\xA3"
#define ICON_BATTERY_4    "\xEE\x86\xA4"
#define ICON_BATTERY_5    "\xEE\x86\xA5"
#define ICON_BATTERY_6    "\xEE\x86\xA6"

#define ICON_BATTERY_CHG  "\xEF\x86\xA3"
#define ICON_POWER_PLUG   "\xEE\x98\xBC"
#define ICON_BOLT         "\xEE\xA8\x8B"

#define ICON_WIFI         "\xEE\x98\xBE"
#define ICON_WIFI_HOTSPOT "\xEE\x87\xA2"
#define ICON_SCHEDULE     "\xEE\xA2\xB5"
#define ICON_TEMP         "\xEE\x87\xBF"
#define ICON_HUMIDITY     "\xEF\xA1\xBE"
#define ICON_SETTINGS     "\xEE\xA2\xB8"
#define ICON_CHECK_CIRCLE "\xEE\xA1\xAC"
#define ICON_VPN_KEY      "\xEE\x83\x9A"
#define ICON_SCREEN       "\xEE\x8C\x8C"
#define ICON_SYNC         "\xEE\x98\xA7"
#define ICON_WARNING      "\xEE\x80\x82"
#define ICON_ERROR        "\xEE\x80\x81"
#define ICON_HOURGLASS    "\xEE\xA9\x9B"
#define ICON_APP_REGISTER "\xEE\xBD\x80"
#define ICON_LOCK_CLOCK   "\xEE\xBD\x97"
#define ICON_MONITOR      "\xEE\xBD\x9B"
#define ICON_PENDING      "\xEF\x86\xBB"
#define ICON_NOTIFICATION "\xEE\x9F\xB4"

#endif /* MATERIAL_ICONS_H_ */