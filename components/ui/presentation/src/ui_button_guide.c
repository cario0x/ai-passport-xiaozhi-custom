// 启动扫码绑定页：二维码携带设备实际 SN 与 Key，右侧任意按键退出。
#include "presentation/ui_button_guide.h"
#include "presentation/ui_theme.h"
#include "services/binding_url.h"

static lv_obj_t *s_scr;
static lv_obj_t *s_prev;

static void open_guide(const char *sn, const char *key, bool from_menu) {
    if (s_scr) return;
    char payload[BINDING_URL_CAP];
    if (binding_url_build(payload, sizeof payload, sn, key) < 0) return;
    s_prev = lv_screen_active();
    s_scr = ui_theme_screen();

    lv_obj_t *brand = ui_theme_text(s_scr, "AI PASSPORT", 14, 10, 212,
                                    &lv_font_montserrat_12, UI_ACCENT);
    lv_obj_set_style_text_letter_space(brand, 2, 0);
    lv_obj_set_style_text_align(brand, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *title = ui_theme_text(s_scr, from_menu ? "连接小程序" : "扫码开启配置", 14, 29, 212,
                                    &ui_font_title, UI_INK);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    // A solid card keeps the QR quiet zone free of decoration and makes
    // scanning independent of the surrounding dark green device theme.
    lv_obj_t *card = lv_obj_create(s_scr);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, 18, 61);
    lv_obj_set_size(card, 204, 204);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *qr = lv_qrcode_create(s_scr);
    lv_qrcode_set_size(qr, 196);
    lv_qrcode_set_dark_color(qr, lv_color_hex(UI_BG));
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_set_quiet_zone(qr, true);
    lv_qrcode_set_data(qr, payload);
    lv_obj_set_pos(qr, 22, 65);

    lv_obj_t *connected = ui_theme_text(s_scr, "蓝牙连上自动进入主页", 14, 273, 212,
                                        &ui_font_caption, UI_INK);
    lv_obj_set_style_text_align(connected, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *skip = ui_theme_text(s_scr, from_menu ? "确定 · 返回玩法" : "右侧任意按键 · 跳过", 14, 296, 212,
                                   &ui_font_caption, UI_MUTED);
    lv_obj_set_style_text_align(skip, LV_TEXT_ALIGN_CENTER, 0);
    lv_screen_load(ui_theme_screen_obj(s_scr));
    lv_obj_set_style_clip_corner(s_scr, false, 0);
    lv_obj_set_style_radius(s_scr, 0, 0);
}

void ui_button_guide_open(const char *sn, const char *key) { open_guide(sn, key, false); }
void ui_button_guide_open_menu(const char *sn, const char *key) { open_guide(sn, key, true); }

bool ui_button_guide_active(void) { return s_scr != NULL; }

void ui_button_guide_close(void) {
    if (!s_scr) return;
    lv_obj_t *old = ui_theme_screen_obj(s_scr);
    if (!old) old = s_scr;
    if (s_prev) lv_screen_load(s_prev);
    s_scr = NULL; s_prev = NULL;
    lv_obj_delete_async(old);
}
