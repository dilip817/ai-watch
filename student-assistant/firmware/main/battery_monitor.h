#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * Initialize AXP2101 battery monitor on shared I2C bus.
 */
esp_err_t battery_monitor_init(void);

/**
 * Read battery percentage (0–100). Returns -1 on error.
 */
int battery_get_percent(void);

/**
 * Check if device is currently charging.
 */
bool battery_is_charging(void);
