// components/platform_esp32/src/disp_driver.c
//
// 统一显示驱动:支持 AI PASSPORT (ST7789) 和 StopWatch (CO5300)
// - AI PASSPORT: 2.00" TFT ST7789P3, 4线SPI, 240×320
// - StopWatch: 1.75" CO5300 AMOLED, QSPI, 466×466 圆形显示
#include "platform/board_config.h"
#if PERIPH_DISPLAY
#include "hal/hal_display.h"
#if BOARD_M5STACK_STOPWATCH
#include "platform/stopwatch_hardware.h"
#endif
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>

#if BOARD_AI_PASSPORT
#  include "esp_lcd_panel_st7789.h"
static const char *TAG = "disp_st7789";
#elif BOARD_M5STACK_STOPWATCH
#  include "esp_lcd_co5300.h"
static const char *TAG = "disp_co5300";
#else
#  error "Unknown board configuration"
#endif

static const char *BLTAG = "disp_bl";

typedef struct {
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t io;
    uint16_t w, h;
#if BOARD_AI_PASSPORT
    int bl_gpio;
    int spi_pins[4]; // CS/SCLK/MOSI/DC
    bool bl_ready;
#elif BOARD_M5STACK_STOPWATCH
    // StopWatch: 背光通过 panel_io 命令控制,无需独立 GPIO
    // M5IOE1 控制显示电源和复位,在 platform_create_display 外部初始化
#endif
} disp_ctx_t;

static void disp_backlight(hal_display_t *self, uint8_t p);

#if BOARD_AI_PASSPORT
// ============================================================================
// AI PASSPORT: ST7789P3 专用逻辑
// ============================================================================

static void st_release_sleep_holds(disp_ctx_t *c) {
    gpio_deep_sleep_hold_dis();
    for (size_t i = 0; i < sizeof(c->spi_pins) / sizeof(c->spi_pins[0]); i++) {
        if (GPIO_IS_VALID_GPIO(c->spi_pins[i])) gpio_hold_dis(c->spi_pins[i]);
    }
}

static void st_hold_spi_sleep_levels(disp_ctx_t *c) {
    for (size_t i = 0; i < sizeof(c->spi_pins) / sizeof(c->spi_pins[0]); i++) {
        int pin = c->spi_pins[i];
        if (!GPIO_IS_VALID_OUTPUT_GPIO(pin)) continue;
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        gpio_set_level(pin, i == 0 ? 1 : 0);
        gpio_hold_en(pin);
    }
    gpio_deep_sleep_hold_en();
}

typedef struct {
    uint8_t  cmd;
    uint8_t  data[16];
    uint8_t  len;
    uint16_t delay_ms;
} st_init_cmd_t;

static const st_init_cmd_t st7789p3_tuning_cmds[] = {
    {0xB2, {0x05, 0x05, 0x00, 0x33, 0x33}, 5, 0},
    {0xB7, {0x35}, 1, 0},
    {0xBB, {0x21}, 1, 0},
    {0xC0, {0x2C}, 1, 0},
    {0xC2, {0x01}, 1, 0},
    {0xC3, {0x0B}, 1, 0},
    {0xC4, {0x20}, 1, 0},
    {0xC6, {0x0F}, 1, 0},
    {0xD0, {0xA7, 0xA1}, 2, 0},
    {0xD0, {0xA4, 0xA1}, 2, 0},
    {0xD6, {0xA1}, 1, 0},
    {0xE0, {0xD0, 0x04, 0x08, 0x0A, 0x09, 0x05, 0x2D, 0x43,
            0x49, 0x09, 0x16, 0x15, 0x26, 0x2B}, 14, 0},
    {0xE1, {0xD0, 0x03, 0x09, 0x0A, 0x0A, 0x06, 0x2E, 0x44,
            0x40, 0x3A, 0x15, 0x15, 0x26, 0x2A}, 14, 10},
};

#define BL_LEDC_TIMER   LEDC_TIMER_0
#define BL_LEDC_MODE    LEDC_LOW_SPEED_MODE
#define BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define BL_LEDC_RES     LEDC_TIMER_10_BIT
#define BL_LEDC_FREQ_HZ 5000

static void bl_init(disp_ctx_t *c) {
    if (c->bl_gpio < 0) { ESP_LOGW(BLTAG, "bl_gpio<0, 背光不受控"); return; }
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(c->bl_gpio);
    ledc_timer_config_t t = {
        .speed_mode      = BL_LEDC_MODE,
        .timer_num       = BL_LEDC_TIMER,
        .duty_resolution = BL_LEDC_RES,
        .freq_hz         = BL_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t e = ledc_timer_config(&t);
    if (e != ESP_OK) { ESP_LOGE(BLTAG, "ledc_timer_config 失败: %d", e); return; }
    ledc_channel_config_t ch = {
        .gpio_num   = c->bl_gpio,
        .speed_mode = BL_LEDC_MODE,
        .channel    = BL_LEDC_CHANNEL,
        .timer_sel  = BL_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    e = ledc_channel_config(&ch);
    if (e != ESP_OK) { ESP_LOGE(BLTAG, "ledc_channel_config 失败: %d", e); return; }
    c->bl_ready = true;
    ESP_LOGI(BLTAG, "背光 LEDC 就绪 gpio=%d", c->bl_gpio);
}

#endif // BOARD_AI_PASSPORT

#if BOARD_M5STACK_STOPWATCH
// ============================================================================
// StopWatch: CO5300 AMOLED 专用逻辑
// ============================================================================

#define LCD_OPCODE_WRITE_CMD (0x02ULL)

// CO5300 初始化序列 (基于上游 xiaozhi-esp32 StopWatch 配置)
static const co5300_lcd_init_cmd_t co5300_init_cmds[] = {
    {0xFE, (uint8_t []){0x00}, 0, 0},
    {0xC4, (uint8_t []){0x80}, 1, 0},
    {0x3A, (uint8_t []){0x55}, 0, 10},  // RGB565
    {0x35, (uint8_t []){0x00}, 0, 10},
    {0x53, (uint8_t []){0x20}, 1, 10},
    {0x51, (uint8_t []){0xFF}, 1, 10},  // 亮度 0xFF (最大)
    {0x63, (uint8_t []){0xFF}, 1, 10},
    {0x2A, (uint8_t []){0x00, 0x00, 0x01, 0xD1}, 4, 0}, // Column 0-465
    {0x2B, (uint8_t []){0x00, 0x00, 0x01, 0xD1}, 4, 0}, // Row 0-465
    {0x11, (uint8_t []){0x00}, 0, 120}, // Exit sleep
    {0x29, (uint8_t []){0x00}, 0, 20},  // Display on
};

#endif // BOARD_M5STACK_STOPWATCH

// ============================================================================
// 通用 HAL 接口实现
// ============================================================================

static int disp_shutdown(hal_display_t *self) {
    disp_ctx_t *c = self ? self->impl : NULL;
    if (!c || !c->panel || !c->io) return -1;

    disp_backlight(self, 0);
    esp_err_t ret = esp_lcd_panel_disp_on_off(c->panel, false);
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_err_t sleep_ret = esp_lcd_panel_io_tx_param(c->io, 0x10, NULL, 0); // SLPIN
    vTaskDelay(pdMS_TO_TICKS(120));

#if BOARD_AI_PASSPORT
    if (c->bl_gpio >= 0) {
        ledc_stop(BL_LEDC_MODE, BL_LEDC_CHANNEL, 0);
        gpio_set_direction(c->bl_gpio, GPIO_MODE_OUTPUT);
        gpio_set_level(c->bl_gpio, 0);
        gpio_hold_en(c->bl_gpio);
        gpio_deep_sleep_hold_en();
    }
    st_hold_spi_sleep_levels(c);
#elif BOARD_M5STACK_STOPWATCH
    // StopWatch: M5IOE1 GPIO 扩展器会在外部 deep sleep 准备时关闭显示电源
#endif

    if (ret != ESP_OK || sleep_ret != ESP_OK) {
        ESP_LOGE(TAG, "显示休眠失败: display=%s sleep=%s",
                 esp_err_to_name(ret), esp_err_to_name(sleep_ret));
        return -1;
    }
    ESP_LOGI(TAG, "显示已执行 DISPOFF + SLPIN");
    return 0;
}

static void disp_flush(hal_display_t *self, int x1, int y1, int x2, int y2, const void *px) {
    disp_ctx_t *c = self->impl;
    esp_lcd_panel_draw_bitmap(c->panel, x1, y1, x2 + 1, y2 + 1, px);
}

static void disp_get_size(hal_display_t *self, uint16_t *w, uint16_t *h) {
    disp_ctx_t *c = self->impl; 
    *w = c->w; 
    *h = c->h;
}

static void disp_backlight(hal_display_t *self, uint8_t p) {
    disp_ctx_t *c = self->impl;
    if (p > 100) p = 100;

#if BOARD_AI_PASSPORT
    uint32_t max_duty = (1u << BL_LEDC_RES) - 1u;
    uint32_t duty = (max_duty * p) / 100u;
    ESP_LOGI(BLTAG, "set_backlight %u%% (ready=%d duty=%u/%u)", p, c->bl_ready, duty, max_duty);
    if (!c->bl_ready) return;
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty);
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
#elif BOARD_M5STACK_STOPWATCH
    // CO5300 AMOLED: 通过命令 0x51 设置亮度
    uint8_t brightness = (255 * p) / 100;
    int lcd_cmd = 0x51;
    lcd_cmd &= 0xff;
    lcd_cmd <<= 8;
    lcd_cmd |= LCD_OPCODE_WRITE_CMD << 24;
    esp_err_t err = esp_lcd_panel_io_tx_param(c->io, lcd_cmd, &brightness, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置亮度失败: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(BLTAG, "set_backlight %u%% (brightness=0x%02X)", p, brightness);
    }
#endif
}

static const hal_display_api_t API = {
    .flush = disp_flush,
    .get_size = disp_get_size,
    .set_backlight = disp_backlight,
    .shutdown = disp_shutdown,
};

// ============================================================================
// 显示驱动工厂函数
// ============================================================================

hal_display_t *platform_create_display(const board_config_t *cfg) {
    disp_ctx_t *c = calloc(1, sizeof(*c));
    c->w = cfg->lcd_w;
    c->h = cfg->lcd_h;

#if BOARD_AI_PASSPORT
    // ========== AI PASSPORT: ST7789P3 4-line SPI ==========
    c->bl_gpio = cfg->lcd_bl;
    c->spi_pins[0] = cfg->lcd_cs;
    c->spi_pins[1] = cfg->lcd_sclk;
    c->spi_pins[2] = cfg->lcd_mosi;
    c->spi_pins[3] = cfg->lcd_dc;
    
    st_release_sleep_holds(c);
    bl_init(c);

    spi_bus_config_t bus = {
        .mosi_io_num = cfg->lcd_mosi,
        .sclk_io_num = cfg->lcd_sclk,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = cfg->lcd_w * 80 * 2,
    };
    spi_bus_initialize(cfg->lcd_spi_host, &bus, SPI_DMA_CH_AUTO);

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = cfg->lcd_cs,
        .dc_gpio_num = cfg->lcd_dc,
        .pclk_hz = cfg->lcd_pclk_hz,
        .spi_mode = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .trans_queue_depth = 10,
    };
    esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)cfg->lcd_spi_host, &io_cfg, &c->io);

    esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = cfg->lcd_rst,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    esp_lcd_new_panel_st7789(c->io, &dev, &c->panel);

    esp_lcd_panel_reset(c->panel);
    esp_lcd_panel_init(c->panel);
    esp_lcd_panel_disp_on_off(c->panel, false);

    // 注入 ST7789P3 调优序列
    for (size_t i = 0; i < sizeof(st7789p3_tuning_cmds) / sizeof(st7789p3_tuning_cmds[0]); i++) {
        const st_init_cmd_t *cmd = &st7789p3_tuning_cmds[i];
        esp_err_t e = esp_lcd_panel_io_tx_param(c->io, cmd->cmd, cmd->data, cmd->len);
        if (e != ESP_OK) ESP_LOGE(TAG, "init cmd 0x%02X 失败: %d", cmd->cmd, e);
        if (cmd->delay_ms) vTaskDelay(pdMS_TO_TICKS(cmd->delay_ms));
    }

    esp_lcd_panel_invert_color(c->panel, LCD_INVERT_COLOR);
    esp_lcd_panel_set_gap(c->panel, 0, 0);

#elif BOARD_M5STACK_STOPWATCH
    // ========== StopWatch: CO5300 QSPI AMOLED ==========
    // M5IOE1 GPIO expander must be initialized before this
    // Perform LCD hardware reset via M5IOE1
    stopwatch_lcd_reset();
    
    spi_bus_config_t bus = {
        .sclk_io_num = cfg->lcd_qspi_sck,
        .data0_io_num = cfg->lcd_qspi_d0,
        .data1_io_num = cfg->lcd_qspi_d1,
        .data2_io_num = cfg->lcd_qspi_d2,
        .data3_io_num = cfg->lcd_qspi_d3,
        .max_transfer_sz = cfg->lcd_w * cfg->lcd_h * sizeof(uint16_t),
        .flags = SPICOMMON_BUSFLAG_QUAD,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(cfg->lcd_spi_host, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = cfg->lcd_qspi_cs,
        .dc_gpio_num = GPIO_NUM_NC,
        .spi_mode = 0,
        .pclk_hz = cfg->lcd_pclk_hz,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags = {
            .quad_mode = true,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)cfg->lcd_spi_host, &io_cfg, &c->io));

    co5300_vendor_config_t vendor_cfg = {
        .init_cmds = co5300_init_cmds,
        .init_cmds_size = sizeof(co5300_init_cmds) / sizeof(co5300_lcd_init_cmd_t),
        .flags = {
            .use_qspi_interface = 1,
        },
    };

    esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = GPIO_NUM_NC, // 复位由 M5IOE1 控制
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(c->io, &dev, &c->panel));

    esp_lcd_panel_set_gap(c->panel, 0x06, 0);
    esp_lcd_panel_reset(c->panel);
    esp_lcd_panel_init(c->panel);
    esp_lcd_panel_invert_color(c->panel, false);
    esp_lcd_panel_mirror(c->panel, false, false);
    esp_lcd_panel_disp_on_off(c->panel, true);

    ESP_LOGI(TAG, "CO5300 466x466 QSPI AMOLED 初始化完成");
#endif

    hal_display_t *d = calloc(1, sizeof(*d));
    d->api = &API;
    d->impl = c;
    return d;
}

esp_lcd_panel_handle_t platform_display_panel(hal_display_t *d) {
    return ((disp_ctx_t*)d->impl)->panel;
}

esp_lcd_panel_io_handle_t platform_display_io(hal_display_t *d) {
    return ((disp_ctx_t*)d->impl)->io;
}

#endif // PERIPH_DISPLAY
