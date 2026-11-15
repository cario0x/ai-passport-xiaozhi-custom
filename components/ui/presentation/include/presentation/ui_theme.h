#pragma once
#include "lvgl.h"

// Shared tokens. User-confirmed Chinese typography: LXGW WenKai Medium,
// native 4bpp. Individual plays keep the user's requested visual directions.
#define UI_BG       0x0E1512
#define UI_PANEL    0x141C18
#define UI_LINE     0x2A3A33
#define UI_INK      0xF2F5F3
#define UI_MUTED    0x707D77
#define UI_ACCENT   0x35E07E
#define UI_GOLD     0xEAC080
#define UI_WARNING  0xE7A185
#define UI_MARGIN  14

LV_FONT_DECLARE(ui_font_body);
LV_FONT_DECLARE(ui_font_title);
LV_FONT_DECLARE(ui_font_answer);
LV_FONT_DECLARE(ui_font_caption);
LV_FONT_DECLARE(ui_font_book);

// Returns the passport content root (240×320). On round displays this may be a
// centered stage whose parent is the full letterbox screen — use
// ui_theme_screen_obj() before lv_screen_load / lv_obj_delete of the screen.
lv_obj_t *ui_theme_screen(void);
lv_obj_t *ui_theme_screen_obj(lv_obj_t *content_root);
lv_obj_t *ui_theme_text(lv_obj_t *parent, const char *text, int x, int y,
                       int width, const lv_font_t *font, uint32_t color);
lv_obj_t *ui_theme_panel(lv_obj_t *parent, int x, int y, int w, int h, bool focused);
void ui_theme_header(lv_obj_t *parent, const char *title, const char *eyebrow);
void ui_theme_footer(lv_obj_t *parent, const char *hint);

// Shared CW2017 gauge. A negative SOC displays the unknown marker.
typedef struct {
    lv_obj_t *icon;
    int level, percent;
} ui_battery_icon_t;
void ui_theme_battery_create(ui_battery_icon_t *battery, lv_obj_t *parent);
void ui_theme_battery_set(ui_battery_icon_t *battery, int percent, int level);
