#include "presentation/ui_theme.h"
#include "presentation/ui_round_layout.h"

lv_obj_t *ui_theme_screen(void) {
    // Content root for passport 240×320 layouts.
    // On round StopWatch panels, LVGL screen objects ignore center-align and
    // stay at (0,0) — physical repro showed the whole blue rect upper-left.
    // Build a full-resolution letterbox screen and return the centered stage
    // as the widget parent. Callers must lv_screen_load(ui_theme_screen_obj()).
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
#if SCREEN_IS_ROUND
    lv_obj_set_size(screen, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_t *stage = ui_round_passport_stage(screen, lv_color_hex(UI_BG));
    if (stage) {
        lv_obj_set_style_text_color(stage, lv_color_hex(UI_INK), 0);
        lv_obj_set_style_text_font(stage, &ui_font_body, 0);
        return stage;
    }
#endif
    ui_round_center_passport_screen(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_text_font(screen, &ui_font_body, 0);
    return screen;
}

lv_obj_t *ui_theme_screen_obj(lv_obj_t *content_root) {
    return content_root ? lv_obj_get_screen(content_root) : NULL;
}
lv_obj_t *ui_theme_text(lv_obj_t *parent, const char *text, int x, int y,
                       int width, const lv_font_t *font, uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_line_space(label, 3, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text);
    return label;
}
lv_obj_t *ui_theme_panel(lv_obj_t *parent, int x, int y, int w, int h, bool focused) {
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_pos(panel, x, y); lv_obj_set_size(panel, w, h);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(focused ? 0x2B3D2D : UI_PANEL), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, focused ? 2 : 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(focused ? UI_ACCENT : UI_LINE), 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}
void ui_theme_header(lv_obj_t *parent, const char *title, const char *eyebrow) {
    ui_theme_text(parent, eyebrow, 14, 10, 212, &lv_font_montserrat_12, UI_MUTED);
    ui_theme_text(parent, title, 14, 29, 212, &ui_font_title, UI_INK);
}
void ui_theme_footer(lv_obj_t *parent, const char *hint) {
    lv_obj_t *label = ui_theme_text(parent, hint, 12, 293, 216, &ui_font_body, UI_MUTED);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
}

// 状态栏电池：22×14 外框 + 2px 正极，1px 边框，内芯距外沿 2px。
#define BATT_BODY_W 22
#define BATT_ICON_H 14
#define BATT_TIP_W  2
#define BATT_TIP_H  6
#define BATT_INSET  2
#define BATT_FILL_W (BATT_BODY_W - 2 * BATT_INSET)
#define BATT_FILL_H (BATT_ICON_H - 2 * BATT_INSET)

static void battery_draw(lv_event_t *event) {
    const ui_battery_icon_t *b = lv_event_get_user_data(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t body; lv_obj_get_coords(b->icon, &body);
    int x = body.x1, y = body.y1;
    body.x2 = x + BATT_BODY_W - 1;
    lv_color_t ink = lv_color_hex(b->level == 0 ? UI_MUTED : b->level == 1 ? 0xE0463A :
                                  b->level == 2 ? 0xF2C14E : UI_INK);
    lv_draw_rect_dsc_t rect; lv_draw_rect_dsc_init(&rect);
    rect.bg_opa = rect.border_opa = LV_OPA_COVER;
    rect.bg_color = lv_obj_get_style_bg_color(lv_obj_get_parent(b->icon), 0); rect.border_color = ink;
    rect.border_width = 1; rect.radius = 2;
    lv_draw_rect(layer, &rect, &body);
    rect.bg_color = ink; rect.border_width = 0; rect.radius = 1;
    lv_area_t tip = { x + BATT_BODY_W, y + (BATT_ICON_H - BATT_TIP_H)/2,
                      x + BATT_BODY_W + BATT_TIP_W - 1, y + (BATT_ICON_H + BATT_TIP_H)/2 - 1 };
    lv_draw_rect(layer, &rect, &tip);
    if (b->percent < 0) {
        lv_area_t dash = { x+7, y+6, x+14, y+7 }; rect.radius = 0;
        lv_draw_rect(layer, &rect, &dash);
    } else if (b->percent > 0) {
        int width = (BATT_FILL_W * b->percent + 99) / 100;
        lv_area_t fill = { x+BATT_INSET, y+BATT_INSET, x+BATT_INSET+width-1,
                           y+BATT_INSET+BATT_FILL_H-1 };
        lv_draw_rect(layer, &rect, &fill);
    }
}
void ui_theme_battery_create(ui_battery_icon_t *b, lv_obj_t *parent) {
    b->percent = -1; b->level = 0;
    b->icon = lv_obj_create(parent);
    lv_obj_remove_style_all(b->icon);
    lv_obj_set_size(b->icon, BATT_BODY_W + BATT_TIP_W, BATT_ICON_H);
    lv_obj_align(b->icon, LV_ALIGN_TOP_RIGHT, -12, 14);
    lv_obj_remove_flag(b->icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b->icon, battery_draw, LV_EVENT_DRAW_MAIN, b);
}
void ui_theme_battery_set(ui_battery_icon_t *b, int percent, int level) {
    if (!b->icon) return;
    if (percent < 0 || level < 1) { percent = -1; level = 0; }
    if (percent > 100) percent = 100;
    if (level > 5) level = 5;
    if (b->percent == percent && b->level == level) return;
    b->percent = percent; b->level = level;
    lv_obj_invalidate(b->icon);
}
