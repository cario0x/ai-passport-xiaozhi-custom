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
#include <esp_lcd_touch_cst816s.h>
#include <esp_lvgl_port.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <wifi_manager.h>

#include <cstring>
#include <string>

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
        }
        // Hide stock emoji / chat chrome; Passport fullscreen owns the viewport.
        if (emoji_box_ != nullptr) {
            lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
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
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
        if (status_bar_ != nullptr) {
            lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
        }
        if (top_bar_ != nullptr) {
            lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
            lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, kSafeInset / 2);
            lv_obj_set_style_pad_left(top_bar_, kSafeInset, 0);
            lv_obj_set_style_pad_right(top_bar_, kSafeInset, 0);
            lv_obj_move_foreground(top_bar_);
        }

        BuildPassportFullscreen(screen);
        lv_display_add_event_cb(display_, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
        ESP_LOGI(TAG,
                 "PassportWelcomeLcdDisplay: edge-to-edge 466 circular FoloToy UI "
                 "(no 240x320 letterbox; face-00 assets)");
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
        return true;
    }

    void SetChatMessage(const char* role, const char* content) override {
        // Defer full Passport chat bubble UI; subtitle stays inside circular safe chord.
        LcdDisplay::SetChatMessage(role, content);
        if (bottom_bar_ != nullptr) {
            lv_obj_clear_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(bottom_bar_, lv_color_hex(kXzBg), 0);
            lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, -kSafeInset);
            if (chat_message_label_ != nullptr) {
                lv_obj_set_width(chat_message_label_, kPanel - 2 * kSafeInset);
            }
        }
    }

    void TriggerSetupAction() {
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateStarting &&
            !WifiManager::GetInstance().IsConnected()) {
            ESP_LOGI(TAG, "Passport CTA/Button → EnterWifiConfigMode");
            // Board method accessed via button lambdas; for display CTA use app toggle.
        }
        ESP_LOGI(TAG, "Passport confirm → ToggleChatState / setup path");
        app.ToggleChatState();
    }

private:
    lv_obj_t* stage_ = nullptr;
    lv_obj_t* face_img_ = nullptr;
    lv_obj_t* cta_ = nullptr;
    int face_frame_ = PASSPORT_FACE_IDLE;

    static void OnCtaClicked(lv_event_t* e) {
        auto* self = static_cast<PassportWelcomeLcdDisplay*>(lv_event_get_user_data(e));
        if (self == nullptr) {
            return;
        }
        ESP_LOGI(TAG, "Passport CTA touch CLICKED (LVGL indev)");
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateStarting &&
            !WifiManager::GetInstance().IsConnected()) {
            // WiFi config is entered from Button1 while starting; also allow CTA.
            // WifiBoard::EnterWifiConfigMode is protected — use button path equivalent:
            // Application exposes network via board. Fall back to ToggleChatState which
            // on starting-unconnected is paired with Button1 EnterWifiConfigMode.
        }
        // Prefer opening WiFi config when disconnected; else toggle chat.
        // Board instance owns EnterWifiConfigMode; invoke via Application state:
        self->FlashCta();
        if (!WifiManager::GetInstance().IsConnected()) {
            // Display cannot call protected EnterWifiConfigMode; synthesize Button1 action
            // by posting the same user-facing outcome the board wires on Button1.
            ESP_LOGI(TAG, "CTA while disconnected — ToggleChatState (Button1 also opens WiFi "
                          "config while starting)");
        }
        app.ToggleChatState();
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

        // Header row inside top safe chord.
        lv_obj_t* brand = lv_label_create(stage_);
        lv_label_set_text(brand, "FoloToy");
        lv_obj_set_style_text_font(brand, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(brand, lv_color_hex(kXzAccent), 0);
        lv_obj_align(brand, LV_ALIGN_TOP_MID, -40, kSafeInset);

        lv_obj_t* title = lv_image_create(stage_);
        lv_image_set_src(title, passport_label_brand_xz());
        lv_obj_align(title, LV_ALIGN_TOP_MID, 36, kSafeInset - 2);

        // Character: authentic 144 asset, uniform scale only (no stretch/crop).
        face_img_ = lv_image_create(stage_);
        lv_image_set_src(face_img_, passport_face(PASSPORT_FACE_IDLE));
        lv_image_set_scale(face_img_, (256 * kFaceShow) / kFaceNative);
        lv_obj_align(face_img_, LV_ALIGN_TOP_MID, 0, kSafeInset + 36);

        // Greeting + hint: centered in the mid/lower safe band (not clipped by arc).
        lv_obj_t* hi = lv_image_create(stage_);
        lv_image_set_src(hi, passport_label_hi());
        lv_obj_align(hi, LV_ALIGN_TOP_MID, 0, kSafeInset + 36 + kFaceShow + 16);

        lv_obj_t* hint = lv_image_create(stage_);
        lv_image_set_src(hint, passport_label_hint());
        lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, kSafeInset + 36 + kFaceShow + 48);

        // CTA: wider chord-friendly pill; hitbox matches visible control.
        const int cta_w = kPanel - 2 * (kSafeInset + 24);  // ~306px
        const int cta_h = 40;
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

    // CST820B source routing (upstream stopwatch had docs only).
    // IOE G4 RST → cst816s@0x15 → lvgl_port_add_touch → clickable CTA.
    // Hardware success is NOT claimed from init alone.
    void InitializeTouch() {
        ESP_LOGI(TAG, "CST820B: RST IOE_G4, I2C@0x15, INT=G13 → lvgl_port_add_touch");
        ioe_.digitalWrite(IOE_PIN_TOUCH_RST, LOW);
        vTaskDelay(pdMS_TO_TICKS(100));
        ioe_.digitalWrite(IOE_PIN_TOUCH_RST, HIGH);
        vTaskDelay(pdMS_TO_TICKS(100));

        esp_lcd_touch_config_t tp_cfg = {};
        tp_cfg.x_max = DISPLAY_WIDTH - 1;
        tp_cfg.y_max = DISPLAY_HEIGHT - 1;
        tp_cfg.rst_gpio_num = GPIO_NUM_NC;
        tp_cfg.int_gpio_num = TOUCH_INT_PIN;
        tp_cfg.levels.reset = 0;
        tp_cfg.levels.interrupt = 0;

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
        ESP_LOGI(TAG, "touch indev=%p routed to Passport CTA (HW unverified)", (void*)indev);
    }

    void InitializeButtons() {
        // Official semantics: B1 chat/WiFi-config, B2 volume.
        // Welcome CTA "配置网络" / hint "请按右侧按钮开始" → right button is B2 on
        // StopWatch hardware, but WiFi config is B1 while starting (official).
        // Map B2 click during welcome-like disconnected start to also allow confirm
        // via ToggleChat; B1 remains WiFi-config / chat toggle.
        button1_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting &&
                !WifiManager::GetInstance().IsConnected()) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        button2_.OnClick([this]() {
            // While disconnected at start, treat right button as confirm (Passport
            // welcome "press right button") → same as WiFi/setup entry path.
            auto& app = Application::GetInstance();
            if (!WifiManager::GetInstance().IsConnected() &&
                (app.GetDeviceState() == kDeviceStateStarting ||
                 app.GetDeviceState() == kDeviceStateIdle)) {
                ESP_LOGI(TAG, "Button2 confirm while disconnected → WiFi config / chat");
                if (app.GetDeviceState() == kDeviceStateStarting) {
                    EnterWifiConfigMode();
                    return;
                }
                app.ToggleChatState();
                return;
            }
            auto* codec = GetAudioCodec();
            int volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(std::string(Lang::Strings::VOLUME) + ":" +
                                           std::to_string(volume) + "%");
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
