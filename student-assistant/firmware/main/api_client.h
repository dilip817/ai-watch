#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

/**
 * Single item result from /ingest endpoint.
 */
typedef struct {
    char item_id[40];
    char type[16];         /* "reminder", "event", "note" */
    char category[16];     /* "insight", "action", "event", "note" */
    char title[128];
    char due_date[16];
    char datetime[20];
    char recurrence[32];
    float confidence;
} ingest_item_t;

/**
 * Multi-item result from /ingest endpoint.
 */
#define MAX_INGEST_ITEMS 10

typedef struct {
    char status[16];       /* "ok", "confirm_needed", "empty", "error" */
    char session_id[40];   /* UUID of the session grouping these items */
    char domain[16];       /* "soccer", "music", "classroom", "general" */
    int  count;            /* number of items in items[] */
    ingest_item_t items[MAX_INGEST_ITEMS];
} ingest_result_t;

/**
 * Callback for streaming audio response chunks.
 * Return ESP_OK to continue, ESP_FAIL to abort.
 */
typedef esp_err_t (*audio_chunk_cb_t)(const uint8_t *data, size_t len, void *ctx);

/**
 * Initialize API client with device ID and server URL.
 */
esp_err_t api_client_init(const char *device_id, const char *server_url);

/**
 * Upload a WAV file to /ingest and parse the JSON response.
 * domain: activity domain string ("soccer", "music", "classroom", "general").
 */
esp_err_t api_ingest_audio(const char *wav_path, const char *domain, ingest_result_t *result);

/**
 * Upload a WAV file to /query and stream the MP3 response via callback.
 * narration_out: if non-NULL, receives the X-Narration header text (up to narration_size bytes).
 */
esp_err_t api_query_audio(const char *wav_path, audio_chunk_cb_t cb, void *ctx,
                          char *narration_out, size_t narration_size);

/**
 * Item from /items endpoint.
 */
typedef struct {
    char id[48];
    char type[16];
    char category[16];
    char domain[16];
    char title[128];
    char due_date[16];
    char datetime[20];
    char recurrence[32];
    int  done;
    int  paused;
} api_item_t;

/**
 * Fetch active items from /items endpoint.
 * Returns number of items written to `out` (max `max_items`), or -1 on error.
 */
int api_fetch_items(api_item_t *out, int max_items);

/**
 * Toggle done/undone for an item. Returns ESP_OK on success.
 */
esp_err_t api_toggle_item(const char *item_id);

/**
 * Toggle pause/unpause for an item. Returns ESP_OK on success.
 */
esp_err_t api_pause_item(const char *item_id);

/**
 * Delete an item. Returns ESP_OK on success.
 */
esp_err_t api_delete_item(const char *item_id);

/**
 * Result from /voice endpoint.
 */
typedef struct {
    char mode[8];       /* "save", "query", "empty", "error" */
    int  item_count;    /* number of items saved (save mode only) */
    char session_id[40];
    char narration[256];
} voice_result_t;

/**
 * Send audio to /voice (unified save+query).
 * Streams WAV response into the provided callback.
 * result: receives mode, item_count, session_id.
 */
esp_err_t api_voice_audio(const char *wav_path, const char *domain,
                           audio_chunk_cb_t cb, void *ctx,
                           voice_result_t *result);

/**
 * Request TTS for text confirmation. Streams WAV response via callback.
 */
esp_err_t api_tts(const char *text, audio_chunk_cb_t cb, void *ctx);
