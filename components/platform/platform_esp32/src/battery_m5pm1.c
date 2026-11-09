// components/platform_esp32/src/battery_m5pm1.c - M5PM1 PMIC battery driver
#include "platform/board_config.h"
#if PERIPH_BATTERY && BOARD_M5STACK_STOPWATCH

#include "platform/platform_factory.h"
#include "platform/stopwatch_hardware.h"
#include "hal/hal_battery.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>

static const char *TAG = "battery_m5pm1";

// Battery voltage range (3.7V Li-Po: 3.4V empty, 4.2V full)
#define BATTERY_MIN_VOLTAGE 3400
#define BATTERY_MAX_VOLTAGE 4200

// HAL battery context
typedef struct {
    atomic_bool reads_enabled;
    atomic_flag io_lock;
    bool first_soc_reported;
} battery_m5pm1_ctx_t;

static battery_m5pm1_ctx_t s_ctx;

// Thread-safe read lock acquisition
static bool begin_read(void) {
    if (!atomic_load(&s_ctx.reads_enabled)) return false;
    if (atomic_flag_test_and_set(&s_ctx.io_lock)) return false;
    if (!atomic_load(&s_ctx.reads_enabled)) {
        atomic_flag_clear(&s_ctx.io_lock);
        return false;
    }
    return true;
}

// HAL interface: Read battery SOC percentage
static int batt_read_soc(hal_battery_t *self) {
    (void)self;
    if (!begin_read()) return -1;
    
    uint16_t voltage_mv = 0;
    int err = stopwatch_read_battery_mv(&voltage_mv);
    
    if (err != 0 || voltage_mv == 0) {
        atomic_flag_clear(&s_ctx.io_lock);
        return -1;
    }
    
    // Linear mapping: 3400mV=0%, 4200mV=100%
    int soc;
    if (voltage_mv < BATTERY_MIN_VOLTAGE) {
        soc = 0;
    } else if (voltage_mv > BATTERY_MAX_VOLTAGE) {
        soc = 100;
    } else {
        soc = ((voltage_mv - BATTERY_MIN_VOLTAGE) * 100) / 
              (BATTERY_MAX_VOLTAGE - BATTERY_MIN_VOLTAGE);
    }
    
    if (!s_ctx.first_soc_reported) {
        s_ctx.first_soc_reported = true;
        ESP_LOGI(TAG, "First SOC reading: %d%%, VBAT=%d mV", soc, voltage_mv);
    }
    
    atomic_flag_clear(&s_ctx.io_lock);
    return soc;
}

// HAL interface: Read battery voltage (mV)
static int batt_read_mv(hal_battery_t *self) {
    (void)self;
    if (!begin_read()) return -1;
    
    uint16_t voltage_mv = 0;
    int err = stopwatch_read_battery_mv(&voltage_mv);
    
    atomic_flag_clear(&s_ctx.io_lock);
    return (err == 0) ? (int)voltage_mv : -1;
}

// HAL interface: Shutdown battery monitoring
static int batt_shutdown(hal_battery_t *self) {
    (void)self;
    atomic_store(&s_ctx.reads_enabled, false);
    
    // Wait for any ongoing read to complete
    while (atomic_flag_test_and_set(&s_ctx.io_lock)) {
        vTaskDelay(1);
    }
    atomic_flag_clear(&s_ctx.io_lock);
    
    ESP_LOGI(TAG, "Battery monitoring shutdown");
    return 0;
}

static const hal_battery_api_t API = {
    .read_soc = batt_read_soc,
    .read_mv = batt_read_mv,
    .shutdown = batt_shutdown,
};

// Factory: Create M5PM1 battery monitor
hal_battery_t *platform_create_battery(const board_config_t *cfg) {
    (void)cfg;
    if (!stopwatch_get_pmic()) {
        ESP_LOGE(TAG, "M5PM1 not initialized - call stopwatch_hardware_init() first");
        return NULL;
    }
    
    // Test read to verify PMIC communication
    uint16_t test_vbat = 0;
    int err = stopwatch_read_battery_mv(&test_vbat);
    if (err != 0) {
        ESP_LOGE(TAG, "M5PM1 readVbat test failed: %d", err);
        return NULL;
    }
    
    ESP_LOGI(TAG, "M5PM1 battery monitor initialized, VBAT=%u mV", test_vbat);
    
    atomic_store(&s_ctx.reads_enabled, true);
    atomic_flag_clear(&s_ctx.io_lock);
    s_ctx.first_soc_reported = false;
    
    hal_battery_t *b = (hal_battery_t*)calloc(1, sizeof(*b));
    b->api = &API;
    b->impl = &s_ctx;
    return b;
}

// Platform-specific: Read charging status
bool platform_battery_get_charging_status(hal_battery_t *b, bool *charging, bool *discharging) {
    (void)b;
    if (!stopwatch_get_pmic()) return false;
    
    // Read M5PM1 GPIO2 (charge status pin)
    // Active low: 0=charging, 1=not charging
    int charge_state = stopwatch_read_charge_status();
    
    if (charge_state < 0) {
        // Read failed, leave status unknown
        if (charging) *charging = false;
        if (discharging) *discharging = false;
        return false;
    }
    
    if (charging) *charging = (charge_state == 0);
    if (discharging) *discharging = (charge_state != 0);
    return true;
}

#endif // PERIPH_BATTERY && BOARD_M5STACK_STOPWATCH

// Fallback when battery monitoring is disabled
#if !PERIPH_BATTERY
#include "platform/platform_factory.h"
hal_battery_t *platform_create_battery(const board_config_t *cfg) { (void)cfg; return NULL; }
#endif
