// components/platform_esp32/include/platform/stopwatch_hardware.h
#pragma once
#include "platform/board_config.h"

#if BOARD_M5STACK_STOPWATCH

#include <driver/i2c_master.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize StopWatch hardware (M5IOE1 + M5PM1)
 * @param i2c_bus I2C master bus handle
 * @return 0 on success, -1 on failure
 */
int stopwatch_hardware_init(i2c_master_bus_handle_t i2c_bus);

/**
 * @brief Get M5IOE1 handle (opaque pointer)
 * @return Pointer to M5IOE1 object, or NULL if not initialized
 */
void* stopwatch_get_ioe(void);

/**
 * @brief Get M5PM1 handle (opaque pointer)
 * @return Pointer to M5PM1 object, or NULL if not initialized
 */
void* stopwatch_get_pmic(void);

/**
 * @brief Perform LCD reset sequence via M5IOE1
 */
void stopwatch_lcd_reset(void);

/**
 * @brief Control speaker amplifier (half-duplex audio)
 * @param enable true to enable, false to disable
 */
void stopwatch_speaker_enable(bool enable);

/**
 * @brief Read battery voltage from M5PM1
 * @param voltage_mv Pointer to receive voltage in millivolts
 * @return 0 on success, -1 on failure
 */
int stopwatch_read_battery_mv(uint16_t *voltage_mv);

/**
 * @brief Read charging status from M5PM1
 * @return Charge pin state (0=charging, 1=not charging, -1=error)
 */
int stopwatch_read_charge_status(void);

#ifdef __cplusplus
}
#endif

#endif // BOARD_M5STACK_STOPWATCH
