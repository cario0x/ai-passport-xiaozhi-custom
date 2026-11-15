#include "wifi_board.h"
#include "backlight.h"
#include "display/lcd_display.h"
#include "esp_lcd_co5300.h"
#include "codecs/es8311_audio_codec.h"
#include "application.h"
#include "button.h"
#include "M5IOE1.h"
#include "M5PM1.h"
#include "config.h"
#include "assets/lang_config.h"
#include "passport_faces.h"
#include "passport_labels.h"

#include <esp_log.h>
#include <esp_lcd_touch.h>
#include <esp_lcd_touch_cst816s.h>
#include <esp_lvgl_port.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <wifi_manager.h>

#include <cstring>
#include <string>

#include <esp_timer.h>

#define TAG "M5StackStopwatch"
#define LCD_OPCODE_WRITE_CMD (0x02ULL)

// Passport visual tokens (from presentation/ui_xiaozhi_theme.h)
static constexpr uint32_t kXzBg = 0x0B1118;
static constexpr uint32_t kXzAccent = 0xF3B584;
static constexpr uint32_t kXzInk = 0xF7EBDD;
// Native 466 circular canvas — no 240×320 letterbox. Content stays inside the
// round safe inset; background fills every pixel of the panel.
static constexpr int kPanel = 466;
static constexpr int kSafeInset = 56;   // matches DISPLAY_ROUND_EDGE_INSET
static constexpr int kFaceNative = 144; // authentic asset size (no stretch)
static constexpr int kFaceShow = 200;   // uniform scale for circular presence

static const co5300_lcd_init_cmd_t vendor_specific_init[] = {
    {0xFE, (uint8_t[]){0x00}, 0, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 0, 10},
    {0x35, (uint8_t[]){0x00}, 0, 10},
    {0x53, (uint8_t[]){0x20}, 1, 10},
    {0x51, (uint8_t[]){0xFF}, 1, 10},
    {0x63, (uint8_t[]){0xFF}, 1, 10},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 0},
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x29, (uint8_t[]){0x00}, 0, 20},
};

// Authentic Passport welcome character UI as an edge-to-edge 466×466 circular
// composition. Passport BG fills the whole panel (no letterbox / black ring).
// Face keeps native proportions (uniform scale only). Text/CTA sit inside the
// circular safe inset. Does NOT restore Passport networking — CTA/buttons call
// official Application / WiFi config.
class PassportWelcomeLcdDisplay : public SpiLcdDisplay {
public:
    static void rounder_event_cb(lv_event_t* e) {
        lv_area_t* area = static_cast<lv_area_t*>(lv_event_get_param(e));
        area->x1 = (area->x1 >> 1) << 1;
        area->y1 = (area->y1 >> 1) << 1;
        area->x2 = ((area->x2 >> 1) << 1) + 1;
        area->y2 = ((area->y2 >> 1) << 1) + 1;
    }

    PassportWelcomeLcdDisplay(esp_lcd_panel_io_handle_t io_handle,
                              esp_lcd_panel_handle_t panel_handle, int width, int height,
                              int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                              bool swap_xy)
        : SpiLcdDisplay(io_handle, panel_handle, width, height, offset_x, offset_y, mirror_x,
                        mirror_y, swap_xy) {}

    void SetupUI() override {
        SpiLcdDisplay::SetupUI();
        DisplayLockGuard lock(this);

        auto screen = lv_screen_active();
        // Full-bleed Passport background — fills every visible circular pixel.
        lv_obj_set_style_bg_color(screen, lv_color_hex(kXzBg), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        if (container_ != nullptr) {
            lv_obj_set_size(container_, kPanel, kPanel);
            lv_obj_set_style_bg_color(container_, lv_color_hex(kXzBg), 0);
            lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
            lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
            // Hidden still defaults to clickable in some LVGL paths — disarm.
            lv_obj_remove_flag(container_, LV_OBJ_FLAG_CLICKABLE);
        }
        // Hide stock emoji / chat chrome; Passport fullscreen owns the viewport.
        if (emoji_box_ != nullptr) {
            lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_CLICKABLE);
        }
        if (emoji_label_ != nullptr) {
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        if (emoji_image_ != nullptr) {
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        }
        if (preview_image_ != nullptr) {
            lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        }
        if (bottom_bar_ != nullptr) {
            // Stock bottom_bar fights the CTA chord and sat behind opaque stage_
            // (wake-hint invisible) — keep it out of the Passport composition.
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_CLICKABLE);
        }

        // Status bar: visible for SetStatus/ShowNotification, but NEVER clickable.
        // a9ad19d put it over the face (kSafeInset+18) → "聆听中..." smeared on avatar
        // (t67u photo) and default CLICKABLE stole upper-screen taps.
        if (status_bar_ != nullptr) {
            lv_obj_clear_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(status_bar_, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(status_bar_, lv_color_hex(kXzBg), 0);
            lv_obj_set_style_bg_opa(status_bar_, LV_OPA_70, 0);
            lv_obj_set_style_radius(status_bar_, 10, 0);
            lv_obj_set_style_pad_left(status_bar_, 8, 0);
            lv_obj_set_style_pad_right(status_bar_, 8, 0);
            lv_obj_set_style_pad_top(status_bar_, 2, 0);
            lv_obj_set_style_pad_bottom(status_bar_, 2, 0);
            // Sit in the top safe chord, ABOVE the face (face starts ~kSafeInset+36).
            lv_obj_set_width(status_bar_, kPanel - 2 * kSafeInset - 40);
            lv_obj_set_height(status_bar_, LV_SIZE_CONTENT);
            lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, kSafeInset - 4);
            if (status_label_ != nullptr) {
                lv_obj_set_width(status_label_, kPanel - 2 * kSafeInset - 56);
                lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_set_style_text_color(status_label_, lv_color_hex(kXzInk), 0);
                lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_CLICKABLE);
                lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
            }
            if (notification_label_ != nullptr) {
                lv_obj_set_width(notification_label_, kPanel - 2 * kSafeInset - 56);
                lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_set_style_text_color(notification_label_, lv_color_hex(kXzInk), 0);
                lv_obj_remove_flag(notification_label_, LV_OBJ_FLAG_CLICKABLE);
                lv_label_set_long_mode(notification_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
            }
        }
        if (top_bar_ != nullptr) {
            lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
            lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 8);
            lv_obj_set_style_pad_left(top_bar_, kSafeInset, 0);
            lv_obj_set_style_pad_right(top_bar_, kSafeInset, 0);
            lv_obj_remove_flag(top_bar_, LV_OBJ_FLAG_CLICKABLE);
        }

        BuildPassportFullscreen(screen);
        // Draw status above stage_ for readability, but it must stay non-clickable
        // so pointer events fall through to CTA / face hit targets.
        if (top_bar_ != nullptr) {
            lv_obj_move_foreground(top_bar_);
        }
        if (status_bar_ != nullptr) {
            lv_obj_move_foreground(status_bar_);
        }
        SyncProvisionUiLocked();
        lv_display_add_event_cb(display_, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
        ESP_LOGI(TAG,
                 "PassportWelcomeLcdDisplay: fullscreen UI (status non-clickable, "
                 "provision CTA gated on !connected)");
    }

    void SetStatus(const char* status) override {
        LvglDisplay::SetStatus(status);
        SyncProvisionUi();
    }

    void UpdateStatusBar(bool update_all = false) override {
        LvglDisplay::UpdateStatusBar(update_all);
        // Periodic: hide 配置网络 once STA is up (t67u stale-UI photo).
        SyncProvisionUi();
    }

    bool SetEmotion(const char* emotion) override {
        // Keep official emotion plumbing for protocol compatibility, then map to
        // Passport face frames instead of stock emoji.
        (void)LcdDisplay::SetEmotion(emotion);
        if (emoji_box_ != nullptr) {
            lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        }
        if (emoji_label_ != nullptr) {
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        if (emoji_image_ != nullptr) {
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        }
        if (stage_ != nullptr) {
            lv_obj_clear_flag(stage_, LV_OBJ_FLAG_HIDDEN);
        }

        int frame = PASSPORT_FACE_IDLE;
        if (emotion != nullptr) {
            if (strcmp(emotion, "happy") == 0 || strcmp(emotion, "laughing") == 0) {
                frame = PASSPORT_FACE_HAPPY;
            } else if (strcmp(emotion, "thinking") == 0 || strcmp(emotion, "confused") == 0) {
                frame = PASSPORT_FACE_THINK;
            } else if (strcmp(emotion, "neutral") == 0 || strcmp(emotion, "relaxed") == 0) {
                frame = PASSPORT_FACE_IDLE;
            } else if (strstr(emotion, "listen") != nullptr) {
                frame = PASSPORT_FACE_LISTEN;
            } else {
                // Speaking / unknown → talk frame as active character cue
                frame = PASSPORT_FACE_TALK;
            }
        }
        SetFaceFrame(frame);
        SyncProvisionUi();
        return true;
    }

    void SetChatMessage(const char* role, const char* content) override {
        // Do NOT unhide stock bottom_bar_ (covers CTA chord / sits behind stage_).
        // Surface system/wake hints via notification toast instead.
        (void)role;
        if (bottom_bar_ != nullptr) {
            DisplayLockGuard lock(this);
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
        if (content != nullptr && content[0] != '\0') {
            ShowNotification(content, 8000);
        }
        SyncProvisionUi();
    }

    // HW evidence (68690f0): CTA click → ToggleChatState while wifi_configuring
    // toggled into audio_testing — user saw "no useful action". CTA must open /
    // surface WiFi provisioning, never the audio-test toggle.
    static void HandleProvisionOrChat(const char* source_tag) {
        auto& app = Application::GetInstance();
        auto& board = static_cast<WifiBoard&>(Board::GetInstance());
        auto state = app.GetDeviceState();
        auto& wifi = WifiManager::GetInstance();

        ESP_LOGI(TAG, "INPUT_%s state=%d connected=%d config_mode=%d", source_tag, (int)state,
                 wifi.IsConnected() ? 1 : 0, wifi.IsConfigMode() ? 1 : 0);

        if (wifi.IsConnected()) {
            ESP_LOGI(TAG, "INPUT_%s → ToggleChatState (connected)", source_tag);
            app.ToggleChatState();
            return;
        }

        // Leave audio-testing if stuck there (prior CTA/B1 mis-wiring).
        if (state == kDeviceStateAudioTesting) {
            ESP_LOGI(TAG, "INPUT_%s → exit audio_testing back to wifi_configuring", source_tag);
            app.ToggleChatState();
            state = app.GetDeviceState();
        }

        if (state == kDeviceStateWifiConfiguring || wifi.IsConfigMode()) {
            std::string hint = Lang::Strings::CONNECT_TO_HOTSPOT;
            hint += wifi.GetApSsid();
            hint += Lang::Strings::ACCESS_VIA_BROWSER;
            hint += wifi.GetApWebUrl();
            board.GetDisplay()->ShowNotification(hint, 10000);
            ESP_LOGI(TAG, "INPUT_%s → wifi_config HINT (AP already up; not ToggleChat)", source_tag);
            return;
        }

        ESP_LOGI(TAG, "INPUT_%s → EnterWifiConfigMode", source_tag);
        board.EnterWifiConfigMode();
    }

    // Hide 配置网络 / 请按右侧按钮 when STA already up (t67u photo: stale provision
    // UI while MQTT connected + 聆听中). Face tap remains for chat when online.
    void SyncProvisionUi() {
        DisplayLockGuard lock(this);
        SyncProvisionUiLocked();
    }

private:
    lv_obj_t* stage_ = nullptr;
    lv_obj_t* face_img_ = nullptr;
    lv_obj_t* hint_img_ = nullptr;
    lv_obj_t* cta_ = nullptr;
    lv_obj_t* online_hit_ = nullptr;
    int face_frame_ = PASSPORT_FACE_IDLE;
    bool provision_visible_ = true;

    void SyncProvisionUiLocked() {
        const bool connected = WifiManager::GetInstance().IsConnected();
        const bool show_provision = !connected;
        if (show_provision == provision_visible_ && cta_ != nullptr) {
            // Still refresh online hitbox enablement below.
        }
        provision_visible_ = show_provision;

        if (cta_ != nullptr) {
            if (show_provision) {
                lv_obj_clear_flag(cta_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(cta_, LV_OBJ_FLAG_CLICKABLE);
            } else {
                lv_obj_add_flag(cta_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(cta_, LV_OBJ_FLAG_CLICKABLE);
            }
        }
        if (hint_img_ != nullptr) {
            if (show_provision) {
                lv_obj_clear_flag(hint_img_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(hint_img_, LV_OBJ_FLAG_HIDDEN);
            }
        }
        // When online, a bottom chord hit target replaces the provision CTA so
        // touch still has a useful action (ToggleChat) without showing 配置网络.
        if (online_hit_ != nullptr) {
            if (show_provision) {
                lv_obj_add_flag(online_hit_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(online_hit_, LV_OBJ_FLAG_CLICKABLE);
            } else {
                lv_obj_clear_flag(online_hit_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(online_hit_, LV_OBJ_FLAG_CLICKABLE);
            }
        }
        if (face_img_ != nullptr) {
            if (show_provision) {
                lv_obj_remove_flag(face_img_, LV_OBJ_FLAG_CLICKABLE);
            } else {
                lv_obj_add_flag(face_img_, LV_OBJ_FLAG_CLICKABLE);
            }
        }
    }

    static void OnCtaClicked(lv_event_t* e) {
        auto* self = static_cast<PassportWelcomeLcdDisplay*>(lv_event_get_user_data(e));
        if (self == nullptr) {
            return;
        }
        ESP_LOGI(TAG, "INPUT_CTA_CLICK (LVGL indev → 配置网络)");
        self->FlashCta();
        HandleProvisionOrChat("CTA");
    }

    static void OnOnlineTouch(lv_event_t* e) {
        auto* self = static_cast<PassportWelcomeLcdDisplay*>(lv_event_get_user_data(e));
        if (self == nullptr) {
            return;
        }
        ESP_LOGI(TAG, "INPUT_ONLINE_TOUCH (face/chord → ToggleChat)");
        HandleProvisionOrChat("ONLINE_TOUCH");
    }

    void FlashCta() {
        if (cta_ == nullptr) {
            return;
        }
        lv_obj_set_style_bg_opa(cta_, LV_OPA_70, 0);
    }

    void SetFaceFrame(int frame) {
        face_frame_ = frame;
        if (face_img_ == nullptr) {
            return;
        }
        DisplayLockGuard lock(this);
        lv_image_set_src(face_img_, passport_face(frame));
    }

    void BuildPassportFullscreen(lv_obj_t* screen) {
        // Full-panel root: Passport BG edge-to-edge (circular bezel crops corners).
        stage_ = lv_obj_create(screen);
        lv_obj_remove_style_all(stage_);
        lv_obj_set_size(stage_, kPanel, kPanel);
        lv_obj_set_pos(stage_, 0, 0);
        lv_obj_set_style_bg_color(stage_, lv_color_hex(kXzBg), 0);
        lv_obj_set_style_bg_opa(stage_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(stage_, 0, 0);
        lv_obj_remove_flag(stage_, LV_OBJ_FLAG_SCROLLABLE);
        // Parent must NOT be clickable — otherwise empty areas swallow presses
        // with no handler and no INPUT_* log (t67u: "无法触摸屏幕").
        lv_obj_remove_flag(stage_, LV_OBJ_FLAG_CLICKABLE);

        // Header row inside top safe chord (below status pill).
        lv_obj_t* brand = lv_label_create(stage_);
        lv_label_set_text(brand, "FoloToy");
        lv_obj_set_style_text_font(brand, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(brand, lv_color_hex(kXzAccent), 0);
        lv_obj_align(brand, LV_ALIGN_TOP_MID, -40, kSafeInset + 18);
        lv_obj_remove_flag(brand, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* title = lv_image_create(stage_);
        lv_image_set_src(title, passport_label_brand_xz());
        lv_obj_align(title, LV_ALIGN_TOP_MID, 36, kSafeInset + 16);
        lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);

        // Character: authentic 144 asset, uniform scale only (no stretch/crop).
        face_img_ = lv_image_create(stage_);
        lv_image_set_src(face_img_, passport_face(PASSPORT_FACE_IDLE));
        lv_image_set_scale(face_img_, (256 * kFaceShow) / kFaceNative);
        lv_obj_align(face_img_, LV_ALIGN_TOP_MID, 0, kSafeInset + 48);
        lv_obj_add_event_cb(face_img_, OnOnlineTouch, LV_EVENT_CLICKED, this);

        // Greeting + hint: centered in the mid/lower safe band (not clipped by arc).
        lv_obj_t* hi = lv_image_create(stage_);
        lv_image_set_src(hi, passport_label_hi());
        lv_obj_align(hi, LV_ALIGN_TOP_MID, 0, kSafeInset + 48 + kFaceShow + 12);
        lv_obj_remove_flag(hi, LV_OBJ_FLAG_CLICKABLE);

        hint_img_ = lv_image_create(stage_);
        lv_image_set_src(hint_img_, passport_label_hint());
        lv_obj_align(hint_img_, LV_ALIGN_TOP_MID, 0, kSafeInset + 48 + kFaceShow + 44);
        lv_obj_remove_flag(hint_img_, LV_OBJ_FLAG_CLICKABLE);

        // CTA: wider chord-friendly pill; hitbox matches visible control.
        const int cta_w = kPanel - 2 * (kSafeInset + 24);  // ~306px
        const int cta_h = 44;
        cta_ = lv_obj_create(stage_);
        lv_obj_remove_style_all(cta_);
        lv_obj_set_size(cta_, cta_w, cta_h);
        lv_obj_align(cta_, LV_ALIGN_BOTTOM_MID, 0, -(kSafeInset + 8));
        lv_obj_set_style_radius(cta_, 14, 0);
        lv_obj_set_style_bg_color(cta_, lv_color_hex(kXzAccent), 0);
        lv_obj_set_style_bg_opa(cta_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(cta_, 0, 0);
        lv_obj_add_flag(cta_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(cta_, OnCtaClicked, LV_EVENT_CLICKED, this);

        lv_obj_t* cta_label = lv_image_create(cta_);
        lv_image_set_src(cta_label, passport_label_cta());
        lv_obj_center(cta_label);
        lv_obj_remove_flag(cta_label, LV_OBJ_FLAG_CLICKABLE);

        // Invisible online chord (same geometry as CTA) — enabled only when connected.
        online_hit_ = lv_obj_create(stage_);
        lv_obj_remove_style_all(online_hit_);
        lv_obj_set_size(online_hit_, cta_w, cta_h + 24);
        lv_obj_align(online_hit_, LV_ALIGN_BOTTOM_MID, 0, -(kSafeInset));
        lv_obj_set_style_bg_opa(online_hit_, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(online_hit_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(online_hit_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(online_hit_, OnOnlineTouch, LV_EVENT_CLICKED, this);
    }
};

class StopwatchBacklight : public Backlight {
public:
    StopwatchBacklight(esp_lcd_panel_io_handle_t panel_io, Display* display)
        : panel_io_(panel_io), display_(display) {}

protected:
    void SetBrightnessImpl(uint8_t brightness) override {
        DisplayLockGuard lock(display_);
        uint8_t data[] = {static_cast<uint8_t>((255 * brightness) / 100)};
        int lcd_cmd = 0x51;
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_CMD << 24;
        esp_err_t err = esp_lcd_panel_io_tx_param(panel_io_, lcd_cmd, data, sizeof(data));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set display brightness: %s", esp_err_to_name(err));
        }
    }

private:
    esp_lcd_panel_io_handle_t panel_io_;
    Display* display_;
};

class M5StackStopwatchBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    M5PM1 pmic_;
    M5IOE1 ioe_;
    Button button1_;
    Button button2_;
    PassportWelcomeLcdDisplay* display_;
    StopwatchBacklight* backlight_;
    esp_lcd_touch_handle_t touch_ = nullptr;

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {.enable_internal_pullup = 1},
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));

        if (ioe_.begin(i2c_bus_, M5IOE1_I2C_ADDR, M5IOE1_I2C_FREQ_100K, M5IOE1_INT_MODE_POLLING) !=
            M5IOE1_OK) {
            ESP_LOGE(TAG, "M5IOE1 begin failed");
            return;
        }
        if (pmic_.begin(i2c_bus_, M5PM1_DEFAULT_ADDR, M5PM1_I2C_FREQ_100K) != M5PM1_OK) {
            ESP_LOGE(TAG, "M5PM1 begin failed");
            return;
        }

        pmic_.setChargeEnable(true);
        pmic_.setBoostEnable(true);
        pmic_.pinMode(PMIC_PIN_CHARGE_STATE, INPUT);
        pmic_.pinMode(PMIC_PIN_CHARGE_PROG, OUTPUT);
        pmic_.digitalWrite(PMIC_PIN_CHARGE_PROG, LOW);

        ioe_.pinMode(IOE_PIN_LCD_POWER, OUTPUT);
        ioe_.setDriveMode(IOE_PIN_LCD_POWER, M5IOE1_DRIVE_PUSHPULL);
        ioe_.digitalWrite(IOE_PIN_LCD_POWER, HIGH);

        ioe_.pinMode(IOE_PIN_LCD_RST, OUTPUT);
        ioe_.setDriveMode(IOE_PIN_LCD_RST, M5IOE1_DRIVE_PUSHPULL);
        ioe_.digitalWrite(IOE_PIN_LCD_RST, HIGH);

        ioe_.pinMode(IOE_PIN_CODEC_POWER, OUTPUT);
        ioe_.setDriveMode(IOE_PIN_CODEC_POWER, M5IOE1_DRIVE_PUSHPULL);
        ioe_.digitalWrite(IOE_PIN_CODEC_POWER, HIGH);

        ioe_.pinMode(IOE_PIN_PA_EN, OUTPUT);
        ioe_.setDriveMode(IOE_PIN_PA_EN, M5IOE1_DRIVE_PUSHPULL);
        ioe_.digitalWrite(IOE_PIN_PA_EN, HIGH);

        ioe_.pinMode(IOE_PIN_MOTOR, OUTPUT);
        ioe_.setDriveMode(IOE_PIN_MOTOR, M5IOE1_DRIVE_PUSHPULL);
        ioe_.digitalWrite(IOE_PIN_MOTOR, LOW);

        ioe_.pinMode(IOE_PIN_TOUCH_RST, OUTPUT);
        ioe_.setDriveMode(IOE_PIN_TOUCH_RST, M5IOE1_DRIVE_PUSHPULL);
        ioe_.digitalWrite(IOE_PIN_TOUCH_RST, HIGH);

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.sclk_io_num = DISPLAY_QSPI_SCK;
        buscfg.data0_io_num = DISPLAY_QSPI_D0;
        buscfg.data1_io_num = DISPLAY_QSPI_D1;
        buscfg.data2_io_num = DISPLAY_QSPI_D2;
        buscfg.data3_io_num = DISPLAY_QSPI_D3;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        buscfg.flags = SPICOMMON_BUSFLAG_QUAD;
        ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_QSPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeDisplay() {
        ioe_.digitalWrite(IOE_PIN_LCD_RST, LOW);
        vTaskDelay(pdMS_TO_TICKS(10));
        ioe_.digitalWrite(IOE_PIN_LCD_RST, HIGH);
        vTaskDelay(pdMS_TO_TICKS(120));

        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_QSPI_CS;
        io_config.dc_gpio_num = GPIO_NUM_NC;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 32;
        io_config.lcd_param_bits = 8;
        io_config.flags.quad_mode = true;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(DISPLAY_QSPI_HOST, &io_config, &panel_io));

        co5300_vendor_config_t vendor_config = {
            .init_cmds = &vendor_specific_init[0],
            .init_cmds_size = sizeof(vendor_specific_init) / sizeof(co5300_lcd_init_cmd_t),
            .flags = {.use_qspi_interface = 1},
        };

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = &vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(panel_io, &panel_config, &panel));

        esp_lcd_panel_set_gap(panel, 0x06, 0);
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, false);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);

        display_ = new PassportWelcomeLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                                 DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                                 DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        backlight_ = new StopwatchBacklight(panel_io, display_);
        backlight_->RestoreBrightness();
    }

    // CST820B: IOE G4 RST → cst816s@0x15 → lvgl_port_add_touch.
    // t67u: INT=G13 EVENT mode inited but no CTA/INPUT_* in 90s while buttons
    // worked. Use TIMER/polling (int=NC) so LVGL reads the CTP without relying
    // on the INT edge — INT pin still free for HW, just not required for indev.
    // Raw touch sampler logs INPUT_RAW_TOUCH to separate "no HW press" vs
    // "press but miss widget". Does NOT claim CTP dead from init alone.
    void InitializeTouch() {
        ESP_LOGI(TAG, "CST820B: RST IOE_G4, I2C@0x15, poll-mode (int=NC) → lvgl_port_add_touch");
        ioe_.digitalWrite(IOE_PIN_TOUCH_RST, LOW);
        vTaskDelay(pdMS_TO_TICKS(100));
        ioe_.digitalWrite(IOE_PIN_TOUCH_RST, HIGH);
        vTaskDelay(pdMS_TO_TICKS(100));

        esp_lcd_touch_config_t tp_cfg = {};
        tp_cfg.x_max = DISPLAY_WIDTH - 1;
        tp_cfg.y_max = DISPLAY_HEIGHT - 1;
        tp_cfg.rst_gpio_num = GPIO_NUM_NC;
        // Polling: LV_INDEV_MODE_TIMER. Keep TOUCH_INT_PIN unused by driver.
        tp_cfg.int_gpio_num = GPIO_NUM_NC;
        tp_cfg.levels.reset = 0;
        tp_cfg.levels.interrupt = 0;
        tp_cfg.flags.swap_xy = 0;
        tp_cfg.flags.mirror_x = 0;
        tp_cfg.flags.mirror_y = 0;
        // Log coords during LVGL's own CTP read (no extra I2C bus contention).
        tp_cfg.process_coordinates =
            [](esp_lcd_touch_handle_t /*tp*/, uint16_t* x, uint16_t* y, uint16_t* strength,
               uint8_t* point_num, uint8_t /*max_point_num*/) {
                if (point_num == nullptr || *point_num == 0 || x == nullptr || y == nullptr) {
                    return;
                }
                static int64_t last_log_us = 0;
                const int64_t now = esp_timer_get_time();
                if (now - last_log_us > 500 * 1000) {
                    last_log_us = now;
                    const unsigned s = (strength != nullptr) ? (unsigned)strength[0] : 0;
                    ESP_LOGI(TAG, "INPUT_RAW_TOUCH x=%u y=%u strength=%u", (unsigned)x[0],
                             (unsigned)y[0], s);
                }
            };

        esp_lcd_panel_io_handle_t tp_io_handle = nullptr;
        esp_lcd_panel_io_i2c_config_t tp_io_config = {};
        tp_io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS;
        tp_io_config.control_phase_bytes = 1;
        tp_io_config.dc_bit_offset = 0;
        tp_io_config.lcd_cmd_bits = 8;
        tp_io_config.lcd_param_bits = 0;
        tp_io_config.flags.disable_control_phase = 1;
        tp_io_config.scl_speed_hz = 400 * 1000;

        esp_err_t err = esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "touch panel_io failed: %s", esp_err_to_name(err));
            return;
        }
        err = esp_lcd_touch_new_i2c_cst816s(tp_io_handle, &tp_cfg, &touch_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "cst816s init failed: %s (not claiming CTP working)", esp_err_to_name(err));
            return;
        }
        lv_display_t* disp = lv_display_get_default();
        if (disp == nullptr) {
            ESP_LOGE(TAG, "no LVGL display for touch indev");
            return;
        }
        const lvgl_port_touch_cfg_t touch_cfg = {.disp = disp, .handle = touch_};
        lv_indev_t* indev = lvgl_port_add_touch(&touch_cfg);
        ESP_LOGI(TAG, "touch indev=%p poll-mode + INPUT_RAW_TOUCH via process_coordinates",
                 (void*)indev);
    }

    void InitializeButtons() {
        // config.h: BUTTON1=GPIO2 (chat/WiFi), BUTTON2=GPIO1 (volume) — matches
        // original official stopwatch.cc. README pin names are swapped vs that.
        // HW 68690f0: volume steps logged while status_bar was hidden → looked dead.
        button1_.OnClick([this]() {
            ESP_LOGI(TAG, "INPUT_BTN1_CLICK gpio=%d", (int)BUTTON1_GPIO);
            auto& app = Application::GetInstance();
            auto state = app.GetDeviceState();
            auto& wifi = WifiManager::GetInstance();

            // Do NOT ToggleChat while provisioning — that enters audio_testing
            // (evidence: wifi_configuring↔audio_testing with no useful UI).
            if (!wifi.IsConnected() || state == kDeviceStateWifiConfiguring ||
                state == kDeviceStateAudioTesting) {
                PassportWelcomeLcdDisplay::HandleProvisionOrChat("BTN1");
                return;
            }
            ESP_LOGI(TAG, "INPUT_BTN1 → ToggleChatState");
            app.ToggleChatState();
        });

        button2_.OnClick([this]() {
            ESP_LOGI(TAG, "INPUT_BTN2_CLICK gpio=%d", (int)BUTTON2_GPIO);
            auto& app = Application::GetInstance();
            auto state = app.GetDeviceState();
            // Right-button confirm while provisioning → same WiFi hint/setup path
            // (Passport "请按右侧按钮开始"), not volume-only.
            if (!WifiManager::GetInstance().IsConnected() &&
                (state == kDeviceStateStarting || state == kDeviceStateIdle ||
                 state == kDeviceStateWifiConfiguring || state == kDeviceStateAudioTesting)) {
                PassportWelcomeLcdDisplay::HandleProvisionOrChat("BTN2");
                return;
            }
            auto* codec = GetAudioCodec();
            int volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            std::string msg = std::string(Lang::Strings::VOLUME) + ":" + std::to_string(volume) + "%";
            ESP_LOGI(TAG, "INPUT_BTN2_VOLUME %d", volume);
            GetDisplay()->ShowNotification(msg, 3000);
        });
    }

public:
    M5StackStopwatchBoard()
        : i2c_bus_(nullptr),
          button1_(BUTTON1_GPIO),
          button2_(BUTTON2_GPIO),
          display_(nullptr),
          backlight_(nullptr) {
        InitializeI2c();
        InitializeSpi();
        InitializeDisplay();
        InitializeTouch();
        InitializeButtons();
    }

    AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_GPIO_PA, AUDIO_CODEC_ES8311_ADDR, false);
        return &audio_codec;
    }

    Display* GetDisplay() override { return display_; }
    Backlight* GetBacklight() override { return backlight_; }

    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        uint16_t voltage_mv = 0;
        if (pmic_.readVbat(&voltage_mv) != M5PM1_OK) {
            return false;
        }
        int charge_state_level = pmic_.digitalRead(PMIC_PIN_CHARGE_STATE);
        if (charge_state_level < 0) {
            charging = false;
            discharging = false;
        } else {
            charging = (charge_state_level == 0);
            discharging = !charging;
        }
        const int BATTERY_MIN_VOLTAGE = 3400;
        const int BATTERY_MAX_VOLTAGE = 4200;
        if (voltage_mv < BATTERY_MIN_VOLTAGE) {
            level = 0;
        } else if (voltage_mv > BATTERY_MAX_VOLTAGE) {
            level = 100;
        } else {
            level = ((voltage_mv - BATTERY_MIN_VOLTAGE) * 100) /
                    (BATTERY_MAX_VOLTAGE - BATTERY_MIN_VOLTAGE);
        }
        return true;
    }
};

DECLARE_BOARD(M5StackStopwatchBoard);
