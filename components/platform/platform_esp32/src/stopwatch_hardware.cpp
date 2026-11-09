// components/platform_esp32/src/stopwatch_hardware.cpp
// StopWatch M5IOE1 GPIO expander and M5PM1 PMIC initialization (C++)
#include "platform/board_config.h"

#if BOARD_M5STACK_STOPWATCH

#include "M5IOE1.h"
#include "M5PM1.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <driver/i2c_master.h>

static const char *TAG = "stopwatch_hw";

// Global C++ objects for M5IOE1 and M5PM1
static M5IOE1 s_ioe;
static M5PM1 s_pmic;
static bool s_initialized = false;

// M5IOE1 I2C address and pin definitions
#define M5IOE1_I2C_ADDR     0x4F
#define M5IOE1_I2C_FREQ     100000  // 100kHz

#define IOE_PIN_LCD_POWER   M5IOE1_PIN_8
#define IOE_PIN_LCD_RST     M5IOE1_PIN_5
#define IOE_PIN_TOUCH_RST   M5IOE1_PIN_4
#define IOE_PIN_CODEC_POWER M5IOE1_PIN_3
#define IOE_PIN_PA_EN       M5IOE1_PIN_10
#define IOE_PIN_MOTOR       M5IOE1_PIN_9

// M5PM1 I2C address
#define M5PM1_I2C_ADDR      0x6E
#define M5PM1_I2C_FREQ      100000  // 100kHz

#define PMIC_PIN_CHARGE_STATE   M5PM1_GPIO_NUM_2
#define PMIC_PIN_CHARGE_PROG    M5PM1_GPIO_NUM_3

extern "C" int stopwatch_hardware_init(i2c_master_bus_handle_t i2c_bus) {
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return 0;
    }

    // Initialize M5IOE1 GPIO expander
    m5ioe1_err_t ioe_err = s_ioe.begin(i2c_bus, M5IOE1_I2C_ADDR, M5IOE1_I2C_FREQ, M5IOE1_INT_MODE_POLLING);
    if (ioe_err != M5IOE1_OK) {
        ESP_LOGE(TAG, "M5IOE1 initialization failed: %d", ioe_err);
        return -1;
    }
    ESP_LOGI(TAG, "M5IOE1 initialized at address 0x%02X", M5IOE1_I2C_ADDR);

    // Initialize M5PM1 PMIC
    m5pm1_err_t pmic_err = s_pmic.begin(i2c_bus, M5PM1_I2C_ADDR, M5PM1_I2C_FREQ);
    if (pmic_err != M5PM1_OK) {
        ESP_LOGE(TAG, "M5PM1 initialization failed: %d", pmic_err);
        return -1;
    }
    ESP_LOGI(TAG, "M5PM1 initialized at address 0x%02X", M5PM1_I2C_ADDR);

    // Configure PMIC
    s_pmic.setChargeEnable(true);
    s_pmic.setBoostEnable(true);
    s_pmic.pinMode(PMIC_PIN_CHARGE_STATE, INPUT);
    s_pmic.pinMode(PMIC_PIN_CHARGE_PROG, OUTPUT);
    s_pmic.digitalWrite(PMIC_PIN_CHARGE_PROG, LOW); // 425mA charge current

    // Configure M5IOE1 GPIO pins
    
    // LCD Power (PYG8)
    s_ioe.pinMode(IOE_PIN_LCD_POWER, OUTPUT);
    s_ioe.setDriveMode(IOE_PIN_LCD_POWER, M5IOE1_DRIVE_PUSHPULL);
    s_ioe.digitalWrite(IOE_PIN_LCD_POWER, HIGH); // Turn on LCD power
    
    // LCD Reset (PYG5) - will be controlled by display driver
    s_ioe.pinMode(IOE_PIN_LCD_RST, OUTPUT);
    s_ioe.setDriveMode(IOE_PIN_LCD_RST, M5IOE1_DRIVE_PUSHPULL);
    s_ioe.digitalWrite(IOE_PIN_LCD_RST, HIGH); // Default high
    
    // Touch Reset (PYG4)
    s_ioe.pinMode(IOE_PIN_TOUCH_RST, OUTPUT);
    s_ioe.setDriveMode(IOE_PIN_TOUCH_RST, M5IOE1_DRIVE_PUSHPULL);
    s_ioe.digitalWrite(IOE_PIN_TOUCH_RST, HIGH); // Keep touch enabled
    
    // Audio Codec Power (PYG3)
    s_ioe.pinMode(IOE_PIN_CODEC_POWER, OUTPUT);
    s_ioe.setDriveMode(IOE_PIN_CODEC_POWER, M5IOE1_DRIVE_PUSHPULL);
    s_ioe.digitalWrite(IOE_PIN_CODEC_POWER, HIGH); // Turn on codec power
    
    // Speaker Amplifier Enable (PYG10) - will be controlled by audio driver
    s_ioe.pinMode(IOE_PIN_PA_EN, OUTPUT);
    s_ioe.setDriveMode(IOE_PIN_PA_EN, M5IOE1_DRIVE_PUSHPULL);
    s_ioe.digitalWrite(IOE_PIN_PA_EN, LOW); // Start disabled for half-duplex
    
    // Motor (PYG9) - keep off by default
    s_ioe.pinMode(IOE_PIN_MOTOR, OUTPUT);
    s_ioe.setDriveMode(IOE_PIN_MOTOR, M5IOE1_DRIVE_PUSHPULL);
    s_ioe.digitalWrite(IOE_PIN_MOTOR, LOW);

    // Wait for power stabilization
    vTaskDelay(pdMS_TO_TICKS(100));

    s_initialized = true;
    ESP_LOGI(TAG, "StopWatch hardware initialization complete");
    return 0;
}

extern "C" void* stopwatch_get_ioe(void) {
    return s_initialized ? (void*)&s_ioe : nullptr;
}

extern "C" void* stopwatch_get_pmic(void) {
    return s_initialized ? (void*)&s_pmic : nullptr;
}

// Display reset sequence via M5IOE1
extern "C" void stopwatch_lcd_reset(void) {
    if (!s_initialized) {
        ESP_LOGE(TAG, "Hardware not initialized");
        return;
    }
    
    s_ioe.digitalWrite(IOE_PIN_LCD_RST, LOW);
    vTaskDelay(pdMS_TO_TICKS(10));
    s_ioe.digitalWrite(IOE_PIN_LCD_RST, HIGH);
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_LOGD(TAG, "LCD reset sequence complete");
}

// Speaker amplifier control (half-duplex audio)
extern "C" void stopwatch_speaker_enable(bool enable) {
    if (!s_initialized) return;
    s_ioe.digitalWrite(IOE_PIN_PA_EN, enable ? HIGH : LOW);
}

// Battery reading from M5PM1
extern "C" int stopwatch_read_battery_mv(uint16_t *voltage_mv) {
    if (!s_initialized || !voltage_mv) return -1;
    return (s_pmic.readVbat(voltage_mv) == M5PM1_OK) ? 0 : -1;
}

// Charging status from M5PM1
extern "C" int stopwatch_read_charge_status(void) {
    if (!s_initialized) return -1;
    return s_pmic.digitalRead(PMIC_PIN_CHARGE_STATE);
}

#endif // BOARD_M5STACK_STOPWATCH
