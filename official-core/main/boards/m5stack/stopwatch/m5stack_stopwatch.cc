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

// CO5300 AMOLED: initialize at full brightness, then restore the saved setting.
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

// Procedural glossy white "Grok Bot" blob on black.
// No prior Grok asset was found in the Passport fork or upstream — this is a
// source-drawn stand-in (opacity-only breath + touch flash). Not a stocked PNG.
class GrokBlobLcdDisplay : public SpiLcdDisplay {
public:
    static void rounder_event_cb(lv_event_t* e) {
        lv_area_t* area = static_cast<lv_area_t*>(lv_event_get_param(e));
        area->x1 = (area->x1 >> 1) << 1;
        area->y1 = (area->y1 >> 1) << 1;
        area->x2 = ((area->x2 >> 1) << 1) + 1;
        area->y2 = ((area->y2 >> 1) << 1) + 1;
    }

    GrokBlobLcdDisplay(esp_lcd_panel_io_handle_t io_handle, esp_lcd_panel_handle_t panel_handle,
                       int width, int height, int offset_x, int offset_y, bool mirror_x,
                       bool mirror_y, bool swap_xy)
        : SpiLcdDisplay(io_handle, panel_handle, width, height, offset_x, offset_y, mirror_x,
                        mirror_y, swap_xy) {}

    void SetupUI() override {
        SpiLcdDisplay::SetupUI();
        DisplayLockGuard lock(this);

        auto screen = lv_screen_active();
        const lv_color_t black = lv_color_hex(0x000000);
        const lv_color_t white = lv_color_hex(0xF5F7FA);
        const lv_color_t soft = lv_color_hex(0xD8DEE8);

        lv_obj_set_style_bg_color(screen, black, 0);
        if (container_ != nullptr) {
            lv_obj_set_style_bg_color(container_, black, 0);
        }
        if (top_bar_ != nullptr) {
            lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
            lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, DISPLAY_ROUND_EDGE_INSET / 2);
            lv_obj_set_style_pad_left(top_bar_, DISPLAY_ROUND_EDGE_INSET, 0);
            lv_obj_set_style_pad_right(top_bar_, DISPLAY_ROUND_EDGE_INSET, 0);
        }
        if (status_bar_ != nullptr) {
            lv_obj_set_style_pad_left(status_bar_, LV_HOR_RES * 0.2, 0);
            lv_obj_set_style_pad_right(status_bar_, LV_HOR_RES * 0.2, 0);
            lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, DISPLAY_STATUS_BAR_TOP_OFF);
            if (status_label_ != nullptr) {
                lv_obj_set_width(status_label_, LV_HOR_RES * 0.6);
                lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);
            }
            if (notification_label_ != nullptr) {
                lv_obj_set_width(notification_label_, LV_HOR_RES * 0.6);
                lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
            }
        }
        if (bottom_bar_ != nullptr) {
            lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, -DISPLAY_CHAT_BAR_BOTTOM_OFF);
            if (chat_message_label_ != nullptr) {
                lv_obj_set_width(chat_message_label_, LV_HOR_RES * 0.75);
            }
        }

        // Hide stock emoji glyph/image; replace with procedural blob.
        if (emoji_label_ != nullptr) {
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        if (emoji_image_ != nullptr) {
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        }

        if (emoji_box_ != nullptr) {
            lv_obj_set_size(emoji_box_, 220, 220);
            lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, -8);
            lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_TRANSP, 0);
            lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(emoji_box_, OnBlobClicked, LV_EVENT_CLICKED, this);

            glow_ = lv_obj_create(emoji_box_);
            lv_obj_remove_style_all(glow_);
            lv_obj_set_size(glow_, 200, 200);
            lv_obj_center(glow_);
            lv_obj_set_style_radius(glow_, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(glow_, soft, 0);
            lv_obj_set_style_bg_opa(glow_, LV_OPA_40, 0);
            lv_obj_remove_flag(glow_, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(glow_, LV_OBJ_FLAG_SCROLLABLE);

            core_ = lv_obj_create(emoji_box_);
            lv_obj_remove_style_all(core_);
            lv_obj_set_size(core_, 148, 148);
            lv_obj_center(core_);
            lv_obj_set_style_radius(core_, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(core_, white, 0);
            lv_obj_set_style_bg_opa(core_, LV_OPA_COVER, 0);
            lv_obj_set_style_shadow_width(core_, 28, 0);
            lv_obj_set_style_shadow_opa(core_, LV_OPA_40, 0);
            lv_obj_set_style_shadow_color(core_, white, 0);
            lv_obj_remove_flag(core_, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(core_, LV_OBJ_FLAG_SCROLLABLE);

            highlight_ = lv_obj_create(emoji_box_);
            lv_obj_remove_style_all(highlight_);
            lv_obj_set_size(highlight_, 54, 28);
            lv_obj_align(highlight_, LV_ALIGN_CENTER, -28, -36);
            lv_obj_set_style_radius(highlight_, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(highlight_, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_bg_opa(highlight_, LV_OPA_70, 0);
            lv_obj_remove_flag(highlight_, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(highlight_, LV_OBJ_FLAG_SCROLLABLE);

            StartBreath(2800);
        }

        lv_display_add_event_cb(display_, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
        ESP_LOGI(TAG, "GrokBlobLcdDisplay SetupUI: centered procedural blob on black (no asset file)");
    }

    bool SetEmotion(const char* emotion) override {
        // Keep official emotion pipeline for logging/theme, then drive blob opacity state.
        bool ok = LcdDisplay::SetEmotion(emotion);
        if (emoji_label_ != nullptr) {
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        if (emoji_image_ != nullptr) {
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        }
        if (emoji_box_ != nullptr) {
            lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        }

        uint32_t period_ms = 2800;
        if (emotion != nullptr) {
            if (strcmp(emotion, "happy") == 0 || strcmp(emotion, "laughing") == 0) {
                period_ms = 1600;
            } else if (strcmp(emotion, "sad") == 0 || strcmp(emotion, "crying") == 0) {
                period_ms = 4200;
            } else if (strcmp(emotion, "angry") == 0 || strcmp(emotion, "surprised") == 0) {
                period_ms = 1200;
            } else if (strcmp(emotion, "thinking") == 0 || strcmp(emotion, "confused") == 0) {
                period_ms = 3400;
            }
        }
        StartBreath(period_ms);
        return ok;
    }

    void FlashTouchFeedback() {
        if (core_ == nullptr) {
            return;
        }
        DisplayLockGuard lock(this);
        lv_anim_delete(core_, nullptr);
        lv_anim_t flash;
        lv_anim_init(&flash);
        lv_anim_set_var(&flash, core_);
        lv_anim_set_values(&flash, LV_OPA_40, LV_OPA_COVER);
        lv_anim_set_time(&flash, 180);
        lv_anim_set_playback_time(&flash, 220);
        lv_anim_set_exec_cb(&flash, [](void* obj, int32_t v) {
            lv_obj_set_style_bg_opa(static_cast<lv_obj_t*>(obj), (lv_opa_t)v, 0);
        });
        lv_anim_set_ready_cb(&flash, [](lv_anim_t* a) {
            auto* self = static_cast<GrokBlobLcdDisplay*>(lv_anim_get_user_data(a));
            if (self != nullptr) {
                self->StartBreath(self->breath_period_ms_);
            }
        });
        lv_anim_set_user_data(&flash, this);
        lv_anim_start(&flash);
    }

private:
    lv_obj_t* glow_ = nullptr;
    lv_obj_t* core_ = nullptr;
    lv_obj_t* highlight_ = nullptr;
    uint32_t breath_period_ms_ = 2800;

    static void OnBlobClicked(lv_event_t* e) {
        auto* self = static_cast<GrokBlobLcdDisplay*>(lv_event_get_user_data(e));
        if (self == nullptr) {
            return;
        }
        ESP_LOGI(TAG, "Blob touch CLICKED → ToggleChatState (LVGL indev routing)");
        self->FlashTouchFeedback();
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateStarting && !WifiManager::GetInstance().IsConnected()) {
            // Board owns WiFi-config entry via Button1; blob only toggles chat when ready.
            return;
        }
        app.ToggleChatState();
    }

    void StartBreath(uint32_t period_ms) {
        breath_period_ms_ = period_ms;
        if (core_ == nullptr) {
            return;
        }
        lv_anim_delete(core_, nullptr);
        lv_anim_delete(glow_, nullptr);

        lv_anim_t breath;
        lv_anim_init(&breath);
        lv_anim_set_var(&breath, core_);
        lv_anim_set_values(&breath, LV_OPA_70, LV_OPA_COVER);
        lv_anim_set_time(&breath, period_ms / 2);
        lv_anim_set_playback_time(&breath, period_ms / 2);
        lv_anim_set_repeat_count(&breath, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&breath, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&breath, [](void* obj, int32_t v) {
            lv_obj_set_style_bg_opa(static_cast<lv_obj_t*>(obj), (lv_opa_t)v, 0);
        });
        lv_anim_start(&breath);

        if (glow_ != nullptr) {
            lv_anim_t halo;
            lv_anim_init(&halo);
            lv_anim_set_var(&halo, glow_);
            lv_anim_set_values(&halo, LV_OPA_20, LV_OPA_50);
            lv_anim_set_time(&halo, period_ms / 2);
            lv_anim_set_playback_time(&halo, period_ms / 2);
            lv_anim_set_repeat_count(&halo, LV_ANIM_REPEAT_INFINITE);
            lv_anim_set_path_cb(&halo, lv_anim_path_ease_in_out);
            lv_anim_set_exec_cb(&halo, [](void* obj, int32_t v) {
                lv_obj_set_style_bg_opa(static_cast<lv_obj_t*>(obj), (lv_opa_t)v, 0);
            });
            lv_anim_start(&halo);
        }
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
    GrokBlobLcdDisplay* display_;
    StopwatchBacklight* backlight_;
    esp_lcd_touch_handle_t touch_ = nullptr;
    bool touch_ready_ = false;

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

        // Touch RST on IOE G4 — prepare before CTP init.
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

        display_ = new GrokBlobLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                          DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                          DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        backlight_ = new StopwatchBacklight(panel_io, display_);
        backlight_->RestoreBrightness();
    }

    // CST820B @ 0x15 on shared SYS_I2C; RST=M5IOE1_G4; INT=GPIO13.
    // Upstream stopwatch documents the part but never initializes CTP.
    // Driver path mirrors Waveshare CST820 boards: esp_lcd_touch_cst816s + lvgl_port_add_touch.
    // Source proves routing: IOE RST pulse → I2C panel_io @0x15 → cst816s → LVGL indev →
    // clickable emoji_box_ → ToggleChatState. Hardware success is NOT claimed here.
    void InitializeTouch() {
        ESP_LOGI(TAG, "CST820B touch: RST pulse on IOE G4, then cst816s@0x15 INT=G13");
        ioe_.digitalWrite(IOE_PIN_TOUCH_RST, LOW);
        vTaskDelay(pdMS_TO_TICKS(100));
        ioe_.digitalWrite(IOE_PIN_TOUCH_RST, HIGH);
        vTaskDelay(pdMS_TO_TICKS(100));

        esp_lcd_touch_config_t tp_cfg = {};
        tp_cfg.x_max = DISPLAY_WIDTH - 1;
        tp_cfg.y_max = DISPLAY_HEIGHT - 1;
        tp_cfg.rst_gpio_num = GPIO_NUM_NC;  // RST already pulsed via IOE
        tp_cfg.int_gpio_num = TOUCH_INT_PIN;
        tp_cfg.levels.reset = 0;
        tp_cfg.levels.interrupt = 0;
        tp_cfg.flags.swap_xy = 0;
        tp_cfg.flags.mirror_x = 0;
        tp_cfg.flags.mirror_y = 0;

        esp_lcd_panel_io_handle_t tp_io_handle = nullptr;
        esp_lcd_panel_io_i2c_config_t tp_io_config = {};
        tp_io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS;  // 0x15
        tp_io_config.control_phase_bytes = 1;
        tp_io_config.dc_bit_offset = 0;
        tp_io_config.lcd_cmd_bits = 8;
        tp_io_config.lcd_param_bits = 0;
        tp_io_config.flags.disable_control_phase = 1;
        tp_io_config.scl_speed_hz = 400 * 1000;

        esp_err_t err = esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "touch panel_io_i2c failed: %s", esp_err_to_name(err));
            return;
        }
        err = esp_lcd_touch_new_i2c_cst816s(tp_io_handle, &tp_cfg, &touch_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_lcd_touch_new_i2c_cst816s failed: %s (CTP not claimed working)",
                     esp_err_to_name(err));
            return;
        }

        lv_display_t* disp = lv_display_get_default();
        if (disp == nullptr) {
            ESP_LOGE(TAG, "No LVGL display for lvgl_port_add_touch");
            return;
        }
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = disp,
            .handle = touch_,
        };
        lv_indev_t* indev = lvgl_port_add_touch(&touch_cfg);
        if (indev == nullptr) {
            ESP_LOGE(TAG, "lvgl_port_add_touch returned null — touch indev not routed");
            return;
        }
        touch_ready_ = true;
        ESP_LOGI(TAG,
                 "CST820B source routing armed: RST→IOE_G4, I2C@0x15, INT=G13, "
                 "lvgl_port_add_touch indev=%p → clickable blob. Hardware not verified.",
                 (void*)indev);
    }

    void InitializeButtons() {
        // Official v2.6.0 semantics retained:
        // Button1 (GPIO2): wake / toggle conversation / WiFi config while starting
        // Button2 (GPIO1): volume cycle
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
