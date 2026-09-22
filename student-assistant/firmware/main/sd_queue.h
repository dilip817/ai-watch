#pragma once

#include "esp_err.h"

/**
 * Initialize SD card (SDMMC) and queue directory.
 * SD card pins from Waveshare factory firmware:
 *   CLK=GPIO2, CMD=GPIO1, DATA=GPIO3
 */
esp_err_t sd_queue_init(void);

/**
 * Add a recording file to the upload queue manifest.
 */
esp_err_t sd_queue_add(const char *filepath);

/**
 * Get the next pending file path from the manifest.
 * Returns NULL if queue is empty. Caller must not free.
 */
const char *sd_queue_peek(void);

/**
 * Mark the oldest pending item as uploaded (removes from queue).
 */
esp_err_t sd_queue_pop(void);

/**
 * Mark the oldest pending item as failed (increment retries).
 * After 5 retries, moves to /sdcard/failed/.
 */
esp_err_t sd_queue_fail(void);

/**
 * Get number of pending items.
 */
int sd_queue_count(void);
