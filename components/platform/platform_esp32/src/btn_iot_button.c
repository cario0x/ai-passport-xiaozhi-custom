// components/platform_esp32/src/btn_iot_button.c
// 统一按钮驱动:支持 ADC 按键 (AI PASSPORT) 和 GPIO 按键 (StopWatch)
#include "platform/board_config.h"
#if PERIPH_BUTTON
#include "hal/hal_button.h"
#include "iot_button.h"
#if BOARD_AI_PASSPORT
#  include "button_adc.h"
#  include "esp_adc/adc_oneshot.h"
#endif
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include <stdlib.h>

#if BOARD_AI_PASSPORT
static const char *TAG = "btn_adc";
#elif BOARD_M5STACK_STOPWATCH
static const char *TAG = "btn_gpio";
#endif

typedef struct btn_ctx_s btn_ctx_t;
// 每个按钮一个转发槽,携带索引,使一套回调能区分 4 个按钮。
typedef struct { btn_ctx_t *parent; int index; } btn_slot_t;
struct btn_ctx_s {
    button_handle_t h[BTN_COUNT];
    btn_slot_t      slot[BTN_COUNT];
    hal_btn_cb_t    cb;
    void           *user;
    int             wake_gpio;
};

static void fwd(void *usr, hal_btn_event_t e) {
    btn_slot_t *s = usr;
    if (s->parent->cb) s->parent->cb(s->index, e, s->parent->user);
}
static void on_press (void *a, void *u) { (void)a; fwd(u, HAL_BTN_PRESS); }
static void on_release(void *a, void *u) { (void)a; fwd(u, HAL_BTN_RELEASE); }
static void on_single(void *a, void *u) { (void)a; fwd(u, HAL_BTN_CLICK); }
static void on_double(void *a, void *u) { (void)a; fwd(u, HAL_BTN_DOUBLE); }
static void on_long  (void *a, void *u) { (void)a; fwd(u, HAL_BTN_LONG); }

static void btn_on_event(hal_button_t *self, hal_btn_cb_t cb, void *user) {
    btn_ctx_t *c = self->impl;
    c->cb = cb; c->user = user;
    for (int i = 0; i < BTN_COUNT; i++) {
        if (!c->h[i]) continue;
        iot_button_register_cb(c->h[i], BUTTON_PRESS_DOWN,        NULL, on_press,  &c->slot[i]);
        iot_button_register_cb(c->h[i], BUTTON_PRESS_UP,          NULL, on_release, &c->slot[i]);
        iot_button_register_cb(c->h[i], BUTTON_SINGLE_CLICK,      NULL, on_single, &c->slot[i]);
        iot_button_register_cb(c->h[i], BUTTON_DOUBLE_CLICK,      NULL, on_double, &c->slot[i]);
        iot_button_register_cb(c->h[i], BUTTON_LONG_PRESS_START,  NULL, on_long,   &c->slot[i]);
    }
}
static int btn_count(hal_button_t *self) { (void)self; return BTN_COUNT; }

static const hal_button_api_t API = { .on_event = btn_on_event, .count = btn_count };

#if BOARD_AI_PASSPORT
// ADC 按键电压窗口(mV)。电路:3.3V — 外部上拉 10k — ADC节点(GPIO0) — 分压电阻 — GND
//   index0 上  :   0Ω  → 3.3×0/10k      =   0mV
//   index1 下  :   1k   → 3.3×1k/11k     ≈ 300mV
//   index2 确定:  2.2k  → 3.3×2.2k/12.2k ≈ 595mV
//   松开       : 上拉到 3.3V(ADC 饱和),不落入任何窗口 → 判为无按下
// 窗口边界取相邻档中点(150/447/1900)。注意:10k 上拉把三档压缩到 0~595mV,
// 相邻间距仅约 300mV,对 ADC 噪声/电阻误差较敏感;若实测误判,改用 2.2k 上拉
// 可把间距拉到 600mV 以上(届时窗口改为 0-500 / 500-1340 / 1340-2200)。
// 注:不能用内部上拉(约 45kΩ 且精度差),会把三档全挤到 0~154mV 且随温漂重叠。
static const uint16_t BTN_MV[BTN_COUNT][2] = {
    {    0,  150 },   // 上   (0mV)
    {  150,  447 },   // 下   (300mV)
    {  447, 1900 },   // 确定 (595mV,上界留宽以隔开松开态的 3300mV)
};
#endif // BOARD_AI_PASSPORT

hal_button_t *platform_create_button(const board_config_t *cfg) {
    btn_ctx_t *c = calloc(1, sizeof(*c));
    c->wake_gpio = cfg->btn_wakeup_gpio;
    
    for (int i = 0; i < BTN_COUNT; i++) {
        c->slot[i].parent = c;
        c->slot[i].index = i;
        const button_config_t bc = { 0 };
        
#if BOARD_AI_PASSPORT
        // AI PASSPORT: ADC 按键
        const button_adc_config_t ac = {
            .adc_handle   = NULL,
            .unit_id      = ADC_UNIT_1,
            .adc_channel  = (uint8_t)cfg->btn_adc_channel,
            .button_index = i,
            .min          = BTN_MV[i][0],
            .max          = BTN_MV[i][1],
        };
        esp_err_t e = iot_button_new_adc_device(&bc, &ac, &c->h[i]);
        if (e != ESP_OK || !c->h[i]) {
            c->h[i] = NULL;
            ESP_LOGE(TAG, "ADC 按键[%d] 创建失败: %s (ch=%d %d~%dmV)",
                     i, esp_err_to_name(e), cfg->btn_adc_channel, BTN_MV[i][0], BTN_MV[i][1]);
        }
#elif BOARD_M5STACK_STOPWATCH
        // StopWatch: GPIO 按键
        const button_gpio_config_t gc = {
            .gpio_num = cfg->btn_gpio[i],
            .active_level = cfg->btn_active_level,
        };
        esp_err_t e = iot_button_create(&bc, &gc, &c->h[i]);
        if (e != ESP_OK || !c->h[i]) {
            c->h[i] = NULL;
            ESP_LOGE(TAG, "GPIO 按键[%d] 创建失败: %s (gpio=%d)",
                     i, esp_err_to_name(e), cfg->btn_gpio[i]);
        } else {
            ESP_LOGI(TAG, "GPIO 按键[%d] 已创建 (gpio=%d active_level=%d)",
                     i, cfg->btn_gpio[i], cfg->btn_active_level);
        }
#endif
    }
    
    hal_button_t *b = calloc(1, sizeof(*b));
    b->api = &API; b->impl = c;
    return b;
}

// 平台层内部:取出第 index 个按钮的底层 iot_button 句柄(供 lvgl_port 接入导航用)。
button_handle_t platform_button_iot_handle(hal_button_t *b, int index) {
    if (!b || index < 0 || index >= BTN_COUNT) return NULL;
    return ((btn_ctx_t *)b->impl)->h[index];
}

bool platform_button_any_pressed(hal_button_t *b) {
    if (!b || !b->impl) return false;
    btn_ctx_t *c = (btn_ctx_t *)b->impl;
    for (int i = 0; i < BTN_COUNT; i++) {
        if (c->h[i] && iot_button_get_key_level(c->h[i])) return true;
    }
    return false;
}

bool platform_button_prepare_deep_sleep(hal_button_t *b) {
    if (!b || !b->impl) return false;
    btn_ctx_t *c = (btn_ctx_t *)b->impl;
    if (c->wake_gpio < 0) return false;

    // 停止按键定时采样
    esp_err_t err = iot_button_stop();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "停止按键采样失败: %s", esp_err_to_name(err));
        return false;
    }

    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << c->wake_gpio,
        .mode = GPIO_MODE_INPUT,
#if BOARD_AI_PASSPORT
        .pull_up_en = GPIO_PULLUP_ENABLE,   // ADC 按键需要上拉
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
#elif BOARD_M5STACK_STOPWATCH
        .pull_up_en = GPIO_PULLUP_ENABLE,   // GPIO 按键也需要上拉
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
#endif
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "配置 GPIO%d 深睡输入失败: %s", c->wake_gpio, esp_err_to_name(err));
        iot_button_resume();
        return false;
    }

    // 等复用和上拉稳定后连续确认高电平
#if BOARD_AI_PASSPORT
    esp_rom_delay_us(1000);
    bool released = gpio_get_level(c->wake_gpio) != 0;
    esp_rom_delay_us(1000);
    released = released && gpio_get_level(c->wake_gpio) != 0;
#elif BOARD_M5STACK_STOPWATCH
    // StopWatch GPIO 按键低电平有效,检查是否松开 (高电平)
    esp_rom_delay_us(1000);
    bool released = gpio_get_level(c->wake_gpio) != 0;
    esp_rom_delay_us(1000);
    released = released && gpio_get_level(c->wake_gpio) != 0;
#endif
    if (!released) {
        ESP_LOGW(TAG, "GPIO%d 入睡前仍为低电平,取消本次深睡", c->wake_gpio);
        iot_button_resume();
        return false;
    }
    return true;
}

void platform_button_cancel_deep_sleep(hal_button_t *b) {
    if (b && b->impl) (void)iot_button_resume();
}

// 开机瞬间读取按键状态,判当前被按住的按键索引,无/松开返回 -1。
// 必须在 platform_create_button **之前**调用。
// 用途:产线自检的"上电按住确定键"触发(不碰电脑,直接在设备完成)。
int platform_button_boot_index(const board_config_t *cfg) {
#if BOARD_AI_PASSPORT
    // AI PASSPORT: ADC 按键
    adc_oneshot_unit_handle_t u = NULL;
    adc_oneshot_unit_init_cfg_t ic = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&ic, &u) != ESP_OK || !u) return -1;
    adc_oneshot_chan_cfg_t cc = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    adc_channel_t ch = (adc_channel_t)cfg->btn_adc_channel;
    int idx = -1;
    if (adc_oneshot_config_channel(u, ch, &cc) == ESP_OK) {
        long acc = 0; int ok = 0;
        for (int i = 0; i < 8; i++) {
            int raw = 0;
            if (adc_oneshot_read(u, ch, &raw) == ESP_OK) { acc += raw; ok++; }
        }
        if (ok) {
            int mv = (int)(acc / ok) * 3300 / 4095;
            for (int i = 0; i < BTN_COUNT; i++)
                if (mv >= BTN_MV[i][0] && mv < BTN_MV[i][1]) { idx = i; break; }
        }
    }
    adc_oneshot_del_unit(u);
    return idx;
#elif BOARD_M5STACK_STOPWATCH
    // StopWatch: GPIO 按键,简单检测哪个被按下 (低电平)
    for (int i = 0; i < BTN_COUNT; i++) {
        int gpio_num = cfg->btn_gpio[i];
        if (!GPIO_IS_VALID_GPIO(gpio_num)) continue;
        
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << gpio_num,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&io) != ESP_OK) continue;
        
        esp_rom_delay_us(100);
        int level = gpio_get_level(gpio_num);
        if (level == cfg->btn_active_level) {
            return i;  // 找到被按下的按键
        }
    }
    return -1;  // 无按键按下
#else
    return -1;
#endif
}
#endif // PERIPH_BUTTON
