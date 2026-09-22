#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    UI_SCREEN_HOME = 0,
    UI_SCREEN_RECORDING,
    UI_SCREEN_PROCESSING,
    UI_SCREEN_SAVED,
    UI_SCREEN_TODAY,
    UI_SCREEN_OFFLINE,
    UI_SCREEN_FINISHED,
    UI_SCREEN_SESSION,   /* Session summary: grouped insights/actions/events */
} ui_screen_t;

/**
 * Initialize LVGL, display driver (CO5300 QSPI), touch (FT3168),
 * create all screens including session summary.
 */
esp_err_t ui_init(void);

/**
 * LVGL thread-safety: lock/unlock before calling any ui_ function
 * from outside the LVGL task.
 */
void ui_lock(void);
void ui_unlock(void);

/**
 * Switch to a screen with fade animation (200ms).
 */
void ui_show_screen(ui_screen_t screen);

/**
 * Same as ui_show_screen but without taking the LVGL mutex.
 * Use ONLY from within LVGL callbacks (timers, animations) that already hold the lock.
 */
void ui_show_screen_nolock(ui_screen_t screen);

/**
 * Update status bar connectivity indicator (WiFi icon color).
 */
void ui_set_connected(bool connected);

/**
 * Update recording waveform bars from RMS level.
 */
void ui_update_waveform(uint16_t rms);

/**
 * Update recording timer display.
 */
void ui_set_rec_timer(uint32_t seconds);

/**
 * Update recording screen hint text (e.g. "Listening — tap to stop").
 */
void ui_set_recording_hint(const char *text);

/**
 * Update processing progress bars (0-100).
 */
void ui_set_progress(int stt_pct, int intent_pct);

/**
 * Set processing screen mode: false = ingest (STT/Intent bars), true = query (Listening...).
 */
void ui_set_processing_mode(bool query_mode);

/**
 * Show query answer text on the processing screen.
 */
void ui_show_query_answer(const char *text);

/**
 * Show saved item result on Panel 3.
 */
void ui_show_saved_item(const char *type, const char *title,
                        const char *date, float confidence);

/**
 * Home screen item — now includes category, recurrence and formatted date.
 */
typedef struct {
    const char *type;        /* "reminder", "event", "note" */
    const char *category;    /* "insight", "action", "event", "note" */
    const char *title;
    const char *date;        /* Pre-formatted ETA e.g. "Apr 18", "Sat 10am", "" */
    const char *recurrence;  /* "daily", "every practice", "" — appended if no date */
    const char *id;          /* item UUID for actions */
    int paused;
    int done;
} ui_item_t;

void ui_set_home_items(const ui_item_t *items, int count);

/**
 * Toggle the RECORD button label between "● RECORD" and "■ STOP".
 * Call from main.c when recording starts/stops.
 */
void ui_set_playing(bool playing);
void ui_set_recording(bool active);

/**
 * Update offline queue count.
 */
void ui_set_queue_count(int count);

/**
 * Item action callback type.
 * action: "done", "pause", "delete"
 */
typedef void (*ui_item_action_cb_t)(const char *item_id, const char *action);
void ui_set_item_action_cb(ui_item_action_cb_t cb);

/* -------------------------------------------------------------------------
 * Action bar callbacks — registered by main.c after ui_init().
 * Called from LVGL event thread; implementations must be ISR-safe
 * (use xTaskNotifyFromISR / xSemaphoreGiveFromISR, not vTaskDelay).
 * -------------------------------------------------------------------------
 */
typedef void (*ui_action_cb_t)(void);

/** Called when user taps RECORD / ■ STOP on the action bar. */
void ui_set_record_cb(ui_action_cb_t cb);

/** Called when user taps LISTEN on the action bar. */
void ui_set_listen_cb(ui_action_cb_t cb);

/** Called when user taps READ ALL on the action bar. */
void ui_set_read_all_cb(ui_action_cb_t cb);

/**
 * Show WiFi provisioning screen with AP SSID and instructions.
 */
void ui_show_provisioning_screen(const char *ap_ssid);

/**
 * Display power control — send AMOLED sleep/wake commands.
 */
void ui_display_sleep(void);   /* 0x28 Display Off + 0x10 Sleep In */
void ui_display_wake(void);    /* 0x11 Sleep Out + 0x29 Display On */

/**
 * Update battery level indicator on status bar.
 * pct: 0–100, charging: true if plugged in.
 */
void ui_set_battery(int pct, bool charging);

/* -------------------------------------------------------------------------
 * Domain selection — 4 pill buttons on home screen
 * -------------------------------------------------------------------------
 */

/** Available activity domains. */
typedef enum {
    UI_DOMAIN_SOCCER    = 0,
    UI_DOMAIN_MUSIC     = 1,
    UI_DOMAIN_CLASSROOM = 2,
    UI_DOMAIN_GENERAL   = 3,
    UI_DOMAIN_COUNT     = 4,
} ui_domain_t;

/** Returns the current domain as a lowercase string (e.g. "soccer"). */
const char *ui_get_domain_str(void);

/** Sets the domain explicitly and updates the pill bar highlight. */
void ui_set_domain(ui_domain_t domain);

/** Returns current domain enum value. */
ui_domain_t ui_get_domain(void);

/* -------------------------------------------------------------------------
 * Session Summary screen (UI_SCREEN_SESSION)
 * -------------------------------------------------------------------------
 */

/** Single item to display in the session summary. */
typedef struct {
    const char *category;   /* "insight", "action", "event", "note" */
    const char *title;
    const char *date;       /* pre-formatted ETA — may be NULL */
    const char *recurrence; /* "daily", "every practice" — may be NULL */
} ui_session_item_t;

/**
 * Populate the session summary screen with extracted items.
 * After calling this, call ui_show_screen(UI_SCREEN_SESSION).
 */
void ui_set_session_items(const char *domain_str,
                          const ui_session_item_t *items, int count);
