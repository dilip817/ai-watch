#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Inactivity timeout before display sleeps (milliseconds) */
#define DISPLAY_TIMEOUT_MS  (60 * 1000)   /* 1 minute */

/**
 * Initialize display power management (inactivity timer).
 * Call after ui_init().
 */
void display_power_init(void);

/**
 * Signal user activity — resets the sleep timer.
 * Call on button press, touch, recording start, backend response, etc.
 * If display is off, wakes it first.
 */
void display_activity_signal(void);

/**
 * Put the display to sleep (AMOLED sleep-in command).
 */
void display_sleep(void);

/**
 * Wake the display (AMOLED sleep-out + display on).
 */
void display_wake(void);

/**
 * Check if display is currently on.
 */
bool display_is_on(void);
