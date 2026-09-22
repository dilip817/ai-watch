#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    WIFI_STATE_INIT = 0,
    WIFI_STATE_PROVISIONING,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_CAPTIVE_PORTAL,
    WIFI_STATE_RECONNECTING,
    WIFI_STATE_OFFLINE,
} wifi_state_t;

typedef void (*wifi_state_cb_t)(wifi_state_t new_state);

/**
 * Initialize WiFi manager. Reads credentials from NVS.
 * If no credentials found, starts SoftAP provisioning.
 */
esp_err_t wifi_manager_init(void);

/**
 * Register a callback for WiFi state changes.
 */
void wifi_manager_register_cb(wifi_state_cb_t cb);

/**
 * Get current WiFi state.
 */
wifi_state_t wifi_manager_get_state(void);

/**
 * Check if device has internet (not just WiFi connection).
 */
bool wifi_manager_has_internet(void);
