#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"

#include "wifi_manager.h"
#include "audio_pipeline.h"
#include "api_client.h"
#include "sd_queue.h"
#include "ui_screens.h"
#include "display_power.h"
#include "battery_monitor.h"
#include "splash_screen.h"

#define BUTTON_GPIO       GPIO_NUM_0   /* Side button (BOOT) */

static const char *TAG = "main";

#define DEVICE_ID    "demo-device"
#define SERVER_URL   "https://student-assistant-api.onrender.com"

/* Event group for coordinating tasks */
static EventGroupHandle_t s_app_events;
#define EVT_UPLOAD_READY  BIT0
#define EVT_WIFI_ONLINE   BIT1

/* Flag: recording just stopped, main loop should upload */
static volatile bool s_upload_pending = false;
/* Flag: upload task is actively uploading — drain task must not touch queue */
static volatile bool s_uploading = false;
/* Flag: next recording is VOICE (query) mode, set by screen VOICE button */
static volatile bool s_listen_mode = false;
/* Flags set by screen action bar buttons (safe to write from LVGL task) */
static volatile bool s_screen_record_pressed  = false;
static volatile bool s_screen_listen_pressed  = false;
static volatile bool s_screen_read_all_pressed = false;
/* Set to abort in-flight audio download (checked in wav_chunk_cb) */
volatile bool g_stop_audio = false;

/* Forward declarations */
static void refresh_home_items(void);

/* ---------- WAV buffer for streaming audio ---------- */
typedef struct {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
} wav_buffer_t;

extern volatile bool g_stop_audio;
static esp_err_t wav_chunk_cb(const uint8_t *data, size_t len, void *ctx)
{
    if (g_stop_audio) { g_stop_audio = false; return ESP_FAIL; }  /* User aborted */
    wav_buffer_t *wb = (wav_buffer_t *)ctx;
    if (wb->len + len > wb->cap) {
        ESP_LOGW(TAG, "WAV buffer full (%u + %u > %u)",
                 (unsigned)wb->len, (unsigned)len, (unsigned)wb->cap);
        return ESP_FAIL;
    }
    memcpy(wb->buf + wb->len, data, len);
    wb->len += len;
    return ESP_OK;
}

/* ---------- TTS confirmation ---------- */
static SemaphoreHandle_t s_tts_sem = NULL;
static char s_tts_text[384] = {0};

/* Format "2026-04-06T10:00" → "April 6th at 10 AM" for speech */
static void format_datetime_for_speech(const char *raw, char *out, size_t out_size)
{
    static const char *month_names[] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    };

    int y, mo, d, h, mi;
    /* Try full datetime: 2026-04-06T10:00 */
    if (sscanf(raw, "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi) == 5) {
        const char *suffix = (d == 1 || d == 21 || d == 31) ? "st" :
                             (d == 2 || d == 22) ? "nd" :
                             (d == 3 || d == 23) ? "rd" : "th";
        const char *mn = (mo >= 1 && mo <= 12) ? month_names[mo - 1] : "???";
        const char *ampm = h >= 12 ? "PM" : "AM";
        int h12 = h % 12;
        if (h12 == 0) h12 = 12;
        if (mi > 0) {
            snprintf(out, out_size, "%s %d%s at %d:%02d %s", mn, d, suffix, h12, mi, ampm);
        } else {
            snprintf(out, out_size, "%s %d%s at %d %s", mn, d, suffix, h12, ampm);
        }
        return;
    }
    /* Try date only: 2026-04-06 */
    if (sscanf(raw, "%d-%d-%d", &y, &mo, &d) == 3) {
        const char *suffix = (d == 1 || d == 21 || d == 31) ? "st" :
                             (d == 2 || d == 22) ? "nd" :
                             (d == 3 || d == 23) ? "rd" : "th";
        const char *mn = (mo >= 1 && mo <= 12) ? month_names[mo - 1] : "???";
        snprintf(out, out_size, "%s %d%s", mn, d, suffix);
        return;
    }
    /* Fallback — use raw string */
    strlcpy(out, raw, out_size);
}

static void queue_tts_confirmation(const char *text)
{
    if (!s_tts_sem || !text || strlen(text) == 0) return;
    strlcpy(s_tts_text, text, sizeof(s_tts_text));
    xSemaphoreGive(s_tts_sem);
}

/* Streaming TTS callback — writes WAV chunks directly to speaker */
static esp_err_t tts_stream_cb(const uint8_t *data, size_t len, void *ctx)
{
    wav_stream_ctx_t *ws = (wav_stream_ctx_t *)ctx;
    return audio_play_wav_stream_write(ws, data, len);
}

static void tts_confirm_task(void *arg)
{
    ESP_LOGI(TAG, "TTS confirmation task started");
    while (1) {
        /* Wait for a confirmation to be queued */
        if (xSemaphoreTake(s_tts_sem, portMAX_DELAY) != pdTRUE) continue;

        /* Check if user is busy (recording or uploading) — skip if so */
        if (audio_is_recording() || s_uploading) {
            ESP_LOGI(TAG, "TTS: user busy, skipping confirmation");
            continue;
        }
        if (!wifi_manager_has_internet()) {
            ESP_LOGI(TAG, "TTS: no internet, skipping confirmation");
            continue;
        }

        ESP_LOGI(TAG, "TTS: streaming confirmation: %s", s_tts_text);
        display_activity_signal();  /* Wake display if sleeping */

        /* Open streaming speaker — audio plays as HTTP chunks arrive */
        wav_stream_ctx_t ws;
        esp_err_t err = audio_play_wav_stream_open(&ws);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "TTS: speaker open failed");
            continue;
        }

        err = api_tts(s_tts_text, tts_stream_cb, &ws);

        audio_play_wav_stream_close(&ws);

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "TTS: fetch failed (err=%d)", err);
        }
    }
}

/* ---------- WiFi state callback ---------- */
static void wifi_state_changed(wifi_state_t new_state)
{
    switch (new_state) {
    case WIFI_STATE_CONNECTED:
        ESP_LOGI(TAG, "WiFi connected");
        ui_set_connected(true);
        xEventGroupSetBits(s_app_events, EVT_WIFI_ONLINE);
        /* Start SNTP time sync (has its own retry mechanism) */
        if (!esp_sntp_enabled()) {
            esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            esp_netif_sntp_init(&config);
            ESP_LOGI(TAG, "SNTP time sync started");
        }
        break;
    case WIFI_STATE_OFFLINE:
    case WIFI_STATE_RECONNECTING:
        ESP_LOGW(TAG, "WiFi offline/reconnecting");
        ui_set_connected(false);
        xEventGroupClearBits(s_app_events, EVT_WIFI_ONLINE);
        break;
    case WIFI_STATE_CAPTIVE_PORTAL:
        ESP_LOGW(TAG, "Captive portal detected");
        ui_set_connected(false);
        break;
    case WIFI_STATE_PROVISIONING:
        ESP_LOGI(TAG, "WiFi provisioning mode — waiting for credentials");
        ui_set_connected(false);
        break;
    default:
        break;
    }
}

/* ---------- Do upload (runs in caller's context) ---------- */
static void do_upload(void)
{
    ESP_LOGI(TAG, "=== Upload starting ===");
    s_uploading = true;

    if (!wifi_manager_has_internet()) {
        ESP_LOGW(TAG, "No internet, file stays in queue");
        s_uploading = false;
        ui_show_screen(UI_SCREEN_HOME);
        return;
    }

    const char *path = sd_queue_peek();
    if (!path) {
        ESP_LOGW(TAG, "No file in queue");
        s_uploading = false;
        ui_show_screen(UI_SCREEN_HOME);
        return;
    }

    ESP_LOGI(TAG, "Uploading: %s (domain=%s)", path, ui_get_domain_str());
    ui_set_progress(50, 0);

    ingest_result_t result = {0};
    esp_err_t err = api_ingest_audio(path, ui_get_domain_str(), &result);
    ESP_LOGI(TAG, "Upload done: err=%d (%s)", err, esp_err_to_name(err));

    if (err == ESP_OK && strcmp(result.status, "error") != 0 && result.count > 0) {
        ui_set_progress(100, 100);

        sd_queue_pop();
        ingest_item_t *first = &result.items[0];
        ESP_LOGI(TAG, "Upload success: %d item(s), first=%s → %s",
                 result.count, first->title, result.status);

        /* Clear uploading flag and queue TTS BEFORE showing saved screen,
         * so TLS handshake + server TTS generation overlap with UI display */
        s_uploading = false;
        {
            char confirm_text[384];
            int pos = 0;

            if (result.count == 1) {
                /* Single item */
                const char *date_str = strlen(first->due_date) > 0 ? first->due_date : first->datetime;
                if (date_str && strlen(date_str) > 0) {
                    char formatted[64];
                    format_datetime_for_speech(date_str, formatted, sizeof(formatted));
                    pos = snprintf(confirm_text, sizeof(confirm_text),
                                   "%s has been added for %s.", first->title, formatted);
                } else {
                    pos = snprintf(confirm_text, sizeof(confirm_text),
                                   "%s has been added.", first->title);
                }
            } else {
                /* Multiple items */
                pos = snprintf(confirm_text, sizeof(confirm_text),
                               "%d items saved. ", result.count);
                for (int i = 0; i < result.count && pos < (int)sizeof(confirm_text) - 40; i++) {
                    ingest_item_t *it = &result.items[i];
                    const char *date_str = strlen(it->due_date) > 0 ? it->due_date : it->datetime;
                    if (date_str && strlen(date_str) > 0) {
                        char formatted[64];
                        format_datetime_for_speech(date_str, formatted, sizeof(formatted));
                        pos += snprintf(confirm_text + pos, sizeof(confirm_text) - pos,
                                        "%s for %s%s ",
                                        it->title, formatted,
                                        i < result.count - 1 ? "," : ".");
                    } else {
                        pos += snprintf(confirm_text + pos, sizeof(confirm_text) - pos,
                                        "%s%s ", it->title,
                                        i < result.count - 1 ? "," : ".");
                    }
                }
            }
            queue_tts_confirmation(confirm_text);
        }

        vTaskDelay(pdMS_TO_TICKS(200));
        if (result.count == 1) {
            /* Single item → saved screen */
            ui_show_saved_item(first->type, first->title,
                               strlen(first->due_date) > 0 ? first->due_date : first->datetime,
                               first->confidence);
            ui_show_screen(UI_SCREEN_SAVED);
            vTaskDelay(pdMS_TO_TICKS(5000));
        } else {
            /* Multiple items → session summary screen */
            ui_session_item_t session_items[MAX_INGEST_ITEMS];
            for (int i = 0; i < result.count; i++) {
                session_items[i].category  = result.items[i].category;
                session_items[i].title     = result.items[i].title;
                session_items[i].date      = strlen(result.items[i].due_date) > 0
                                               ? result.items[i].due_date
                                               : result.items[i].datetime;
                session_items[i].recurrence = result.items[i].recurrence;
            }
            ui_set_session_items(result.domain[0] ? result.domain : ui_get_domain_str(),
                                 session_items, result.count);
            ui_show_screen(UI_SCREEN_SESSION);
            /* Auto-return to home after 10 seconds if user doesn't interact */
            vTaskDelay(pdMS_TO_TICKS(10000));
        }
        refresh_home_items();
        ui_show_screen(UI_SCREEN_HOME);
    } else {
        ESP_LOGW(TAG, "Upload failed for %s (err=%d)", path, err);
        sd_queue_fail();
        s_uploading = false;
        ui_show_screen(UI_SCREEN_HOME);
    }
}

/* ---------- Ask/Query flow ---------- */
/* wav_buffer_t and wav_chunk_cb defined above (shared with TTS confirmation) */

static void do_query(void)
{
    ESP_LOGI(TAG, "=== Query (Ask) starting ===");
    s_uploading = true;

    if (!wifi_manager_has_internet()) {
        ESP_LOGW(TAG, "No internet for query");
        s_uploading = false;
        ui_show_screen(UI_SCREEN_HOME);
        return;
    }

    const char *path = sd_queue_peek();
    if (!path) {
        ESP_LOGW(TAG, "No file in queue for query");
        s_uploading = false;
        ui_show_screen(UI_SCREEN_HOME);
        return;
    }

    ESP_LOGI(TAG, "Querying: %s", path);

    /* Switch processing screen to query/listening mode */
    ui_set_processing_mode(true);

    static char narration[512];
    narration[0] = '\0';

    /* Allocate buffer in PSRAM for WAV response (up to 1MB) */
    wav_buffer_t wb = { .buf = NULL, .len = 0, .cap = 1024 * 1024 };
    wb.buf = heap_caps_malloc(wb.cap, MALLOC_CAP_SPIRAM);
    if (!wb.buf) {
        ESP_LOGE(TAG, "Failed to allocate WAV buffer");
        s_uploading = false;
        ui_show_screen(UI_SCREEN_HOME);
        return;
    }

    voice_result_t vr __attribute__((unused)) = {0};
    esp_err_t err = api_voice_audio(path, ui_get_domain_str(), wav_chunk_cb, &wb, &vr);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Voice completed: %u bytes WAV, mode=%s",
                 (unsigned)wb.len, vr.mode);
        sd_queue_pop();

        /* Show speaker icon during playback */
        ui_show_query_answer(NULL);  /* Voice always plays audio */

        /* Play WAV audio through speaker */
        if (wb.len > 44) {
            ESP_LOGI(TAG, "Playing %u bytes WAV audio", (unsigned)wb.len);
            ui_set_playing(true);
            esp_err_t play_err = audio_play_wav(wb.buf, wb.len);
            ui_set_playing(false);
            if (play_err != ESP_OK) {
                ESP_LOGW(TAG, "WAV playback stopped or failed: %s", esp_err_to_name(play_err));
            }
        } else {
            ESP_LOGW(TAG, "WAV response too small (%u bytes)", (unsigned)wb.len);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    } else {
        ESP_LOGW(TAG, "Query failed (err=%d)", err);
        sd_queue_fail();
        ui_show_query_answer("Query failed. Try again.");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    free(wb.buf);

    /* Go straight to home — don't reset processing mode here,
     * it will be set correctly next time the processing screen is shown */
    s_uploading = false;
    refresh_home_items();
    ui_show_screen(UI_SCREEN_HOME);
}

/* ---------- Upload task ----------
 * Waits for notification bits: 0x01 = ingest, 0x02 = query
 */
static void upload_task(void *arg)
{
    ESP_LOGI(TAG, "Upload task running (stack high water: %u)",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    while (1) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, 0xFFFFFFFF, &bits, portMAX_DELAY);
        ESP_LOGI(TAG, "Upload task notified (bits=0x%lx)", (unsigned long)bits);

        if (bits & 0x02) {
            do_query();
        } else {
            do_upload();
        }
    }
}

static TaskHandle_t s_upload_task_handle = NULL;

/* ---------- Date formatting helper ---------- */

static const char *MONTH_ABBR[] = {
    "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};

/**
 * Format a display date from ISO strings.
 * "2026-04-18"       → "Apr 18"
 * "2026-04-18T10:00" → "Apr 18 10am"
 * ""                 → ""  (returns empty string)
 */
static void format_display_date(const char *due_date, const char *datetime,
                                 char *out, size_t out_sz)
{
    out[0] = '\0';
    const char *src = NULL;
    if (due_date && due_date[0])    src = due_date;
    else if (datetime && datetime[0]) src = datetime;
    if (!src) return;

    /* Parse YYYY-MM-DD */
    int y = 0, m = 0, d = 0, hh = 0, mm = 0;
    int have_time = 0;
    if (sscanf(src, "%d-%d-%d", &y, &m, &d) < 3) return;
    if (sscanf(src, "%d-%d-%dT%d:%d", &y, &m, &d, &hh, &mm) == 5) have_time = 1;

    if (m < 1 || m > 12) return;
    if (have_time) {
        int h12 = hh % 12;
        if (h12 == 0) h12 = 12;
        const char *ampm = hh >= 12 ? "pm" : "am";
        if (mm == 0) {
            snprintf(out, out_sz, "%s %d %d%s", MONTH_ABBR[m-1], d, h12, ampm);
        } else {
            snprintf(out, out_sz, "%s %d %d:%02d%s", MONTH_ABBR[m-1], d, h12, mm, ampm);
        }
    } else {
        snprintf(out, out_sz, "%s %d", MONTH_ABBR[m-1], d);
    }
}

/* ---------- Last items copy for Read All ---------- */
#define LAST_ITEMS_MAX 15
static api_item_t s_last_items[LAST_ITEMS_MAX];
static int        s_last_items_count = 0;

/* ---------- Refresh home screen items from backend ---------- */
static void refresh_home_items(void)
{
    if (!wifi_manager_has_internet()) return;

    static api_item_t items[LAST_ITEMS_MAX];
    int count = api_fetch_items(items, LAST_ITEMS_MAX);
    if (count < 0) return;

    /* Keep a copy for Read All */
    s_last_items_count = count < LAST_ITEMS_MAX ? count : LAST_ITEMS_MAX;
    memcpy(s_last_items, items, s_last_items_count * sizeof(api_item_t));

    /* Convert to UI items with formatted dates */
    static ui_item_t   ui_items[LAST_ITEMS_MAX];
    static char        ui_dates[LAST_ITEMS_MAX][24];

    for (int i = 0; i < count && i < LAST_ITEMS_MAX; i++) {
        format_display_date(items[i].due_date, items[i].datetime,
                            ui_dates[i], sizeof(ui_dates[i]));
        ui_items[i].type       = items[i].type;
        ui_items[i].category   = items[i].category[0] ? items[i].category : items[i].type;
        ui_items[i].title      = items[i].title;
        ui_items[i].date       = ui_dates[i];
        ui_items[i].recurrence = items[i].recurrence;
        ui_items[i].id         = items[i].id;
        ui_items[i].paused     = items[i].paused;
        ui_items[i].done       = items[i].done;
    }
    ui_set_home_items(ui_items, count);
    ESP_LOGI(TAG, "Home items refreshed: %d items", count);
}


/* ---------- Drain task ----------
 * Periodically drains the SD queue and refreshes home items.
 */
/* ---------- Battery monitor task ---------- */
static void battery_task(void *arg)
{
    while (1) {
        int pct = battery_get_percent();
        bool charging = battery_is_charging();
        if (pct >= 0) {
            ui_set_battery(pct, charging);
        }
        vTaskDelay(pdMS_TO_TICKS(30000));  /* Update every 30s */
    }
}

static void drain_task(void *arg)
{
    ESP_LOGI(TAG, "Drain task started");

    /* Initial item fetch after 5 seconds */
    vTaskDelay(pdMS_TO_TICKS(5000));
    refresh_home_items();

    while (1) {
        /* Poll faster when display is on, slower when sleeping */
        int delay_s = display_is_on() ? 30 : 600;  /* 30s active, 10min sleeping */
        vTaskDelay(pdMS_TO_TICKS(delay_s * 1000));

        if (!wifi_manager_has_internet()) continue;

        /* Drain pending uploads — always process queue even when sleeping */
        if (sd_queue_count() > 0 && !s_uploading) {
            ESP_LOGI(TAG, "Draining queue: %d pending", sd_queue_count());

            const char *path = sd_queue_peek();
            if (path) {
                ingest_result_t result = {0};
                esp_err_t err = api_ingest_audio(path, ui_get_domain_str(), &result);

                if (err == ESP_OK && strcmp(result.status, "error") != 0) {
                    sd_queue_pop();
                    ESP_LOGI(TAG, "Drain: uploaded %d item(s)", result.count);
                } else {
                    sd_queue_fail();
                    ESP_LOGW(TAG, "Drain: failed %s", path);
                }

                ui_set_queue_count(sd_queue_count());
            }
        }

        /* Refresh home items */
        refresh_home_items();
    }
}

/* ---------- Waveform update task ----------
 * Updates UI waveform bars every 100ms while recording.
 */
static void waveform_task(void *arg)
{
    uint32_t rec_start = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));

        if (audio_is_recording()) {
            uint16_t rms = audio_get_rms();
            ui_update_waveform(rms);

            if (rec_start == 0) rec_start = xTaskGetTickCount();
            uint32_t elapsed = (xTaskGetTickCount() - rec_start) * portTICK_PERIOD_MS / 1000;
            ui_set_rec_timer(elapsed);
        } else {
            if (rec_start != 0) {
                rec_start = 0;
            }
        }
    }
}

/* ---------- Item action callback (from UI touch) ---------- */

/* Item action runs in a short-lived task so it doesn't block the LVGL thread */
typedef struct {
    char item_id[64];
    char action[8];
} pending_action_t;

static void item_action_task(void *arg)
{
    pending_action_t *pa = (pending_action_t *)arg;
    ESP_LOGI(TAG, "Item action task: %s → %s", pa->item_id, pa->action);

    esp_err_t err = ESP_FAIL;
    if (strcmp(pa->action, "done") == 0) {
        err = api_toggle_item(pa->item_id);
    } else if (strcmp(pa->action, "pause") == 0) {
        err = api_pause_item(pa->item_id);
    } else if (strcmp(pa->action, "delete") == 0) {
        err = api_delete_item(pa->item_id);
    }

    if (err == ESP_OK) {
        refresh_home_items();
    } else {
        ESP_LOGW(TAG, "Item action failed: %s", pa->action);
    }

    free(pa);
    vTaskDelete(NULL);
}

static void on_item_action(const char *item_id, const char *action)
{
    if (!item_id || !action) return;
    ESP_LOGI(TAG, "Item action: %s → %s (spawning task)", item_id, action);

    pending_action_t *pa = heap_caps_malloc(sizeof(pending_action_t), MALLOC_CAP_SPIRAM);
    if (!pa) { ESP_LOGE(TAG, "No mem for action"); return; }
    strlcpy(pa->item_id, item_id, sizeof(pa->item_id));
    strlcpy(pa->action, action, sizeof(pa->action));

    xTaskCreateWithCaps(item_action_task, "item_act", 6144, pa,
                        5, NULL, MALLOC_CAP_SPIRAM);
}

/* ---------- Splash screen done callback ---------- */
/* NOTE: This runs inside lv_timer_handler() — LVGL mutex already held.
 * Must use ui_show_screen_nolock() to avoid deadlock. */
static void on_splash_done(void)
{
    ESP_LOGI(TAG, "Splash done — loading home screen");
    if (wifi_manager_get_state() != WIFI_STATE_PROVISIONING) {
        ui_show_screen_nolock(UI_SCREEN_HOME);
    }
}

/* ---------- Action bar button callbacks (called from LVGL event thread) ---------- */

static void on_btn_record(void)
{
    if (audio_is_playing()) {
        audio_stop_playback();  /* Stop playback immediately */
    } else if (s_uploading) {
        g_stop_audio = true;  /* Abort download */
    }
    s_screen_record_pressed = true;
}

static void on_btn_listen(void)
{
    s_screen_listen_pressed = true;   /* Handled in main loop */
}

static void on_btn_read_all(void)
{
    /* VIEW ALL — show the All Items screen */
    s_screen_read_all_pressed = true;
}

/* ---------- App main ---------- */

void app_main(void)
{
    ESP_LOGI(TAG, "=== Student Assistant ===");
    ESP_LOGI(TAG, "Device: Waveshare ESP32-S3 2.06\" AMOLED");
    ESP_LOGI(TAG, "Display: CO5300 QSPI 410x502");
    ESP_LOGI(TAG, "Free heap: %lu, free internal: %lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    /* NVS init */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* Event loop */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* App event group */
    s_app_events = xEventGroupCreate();

    /* Initialize display and UI (must be first for visual feedback) */
    ESP_LOGI(TAG, "Initializing UI...");
    ui_init();
    display_power_init();
    ui_set_item_action_cb(on_item_action);
    ui_set_record_cb(on_btn_record);
    ui_set_listen_cb(on_btn_listen);
    ui_set_read_all_cb(on_btn_read_all);

    /* Show splash screen — animation plays during remaining init */
    splash_screen_set_done_cb(on_splash_done);
    ui_lock();
    splash_screen_show();
    ui_unlock();

    /* Initialize battery monitor (AXP2101 on shared I2C) */
    if (battery_monitor_init() == ESP_OK) {
        int pct = battery_get_percent();
        ui_set_battery(pct, battery_is_charging());
    }

    /* Initialize SD card */
    ESP_LOGI(TAG, "Initializing SD card...");
    sd_queue_init();

    /* Initialize WiFi */
    ESP_LOGI(TAG, "Initializing WiFi...");
    wifi_manager_register_cb(wifi_state_changed);
    wifi_manager_init();

    /* Initialize audio pipeline */
    ESP_LOGI(TAG, "Initializing audio...");
    audio_pipeline_init();

    /* Initialize API client */
    api_client_init(DEVICE_ID, SERVER_URL);

    /* Launch tasks */
    ESP_LOGI(TAG, "Launching tasks...");
    ESP_LOGI(TAG, "Free heap before tasks: %lu internal: %lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    /* Upload task — 10KB stack in PSRAM (internal RAM too tight) */
    BaseType_t xr;
    xr = xTaskCreateWithCaps(upload_task, "upload", 10 * 1024, NULL, 3,
                              &s_upload_task_handle, MALLOC_CAP_SPIRAM);
    if (xr != pdPASS) {
        ESP_LOGE(TAG, "FAILED to create upload task! err=%d", xr);
        ESP_LOGE(TAG, "Free PSRAM: %lu internal: %lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    } else {
        ESP_LOGI(TAG, "Upload task created OK (PSRAM stack)");
    }

    /* Drain task — 10KB stack in PSRAM (needs TLS for HTTP) */
    xr = xTaskCreateWithCaps(drain_task, "drain", 10 * 1024, NULL, 1,
                              NULL, MALLOC_CAP_SPIRAM);
    if (xr != pdPASS) {
        ESP_LOGW(TAG, "Drain task skipped — uploads still work via upload task");
    } else {
        ESP_LOGI(TAG, "Drain task created OK (PSRAM stack)");
    }

    xr = xTaskCreate(waveform_task, "waveform", 3072, NULL, 2, NULL);
    if (xr != pdPASS) {
        ESP_LOGE(TAG, "FAILED to create waveform task! err=%d", xr);
    }

    /* audio_capture_task (priority 5) and vad_task (priority 4)
     * are launched inside audio_pipeline_init() */

    /* TTS confirmation task — low priority, PSRAM stack (needs TLS) */
    s_tts_sem = xSemaphoreCreateBinary();
    xr = xTaskCreateWithCaps(tts_confirm_task, "tts_confirm", 10 * 1024, NULL, 1,
                              NULL, MALLOC_CAP_SPIRAM);
    if (xr != pdPASS) {
        ESP_LOGW(TAG, "TTS confirmation task skipped");
    } else {
        ESP_LOGI(TAG, "TTS confirmation task created OK");
    }

    /* Battery monitor task — lightweight, small stack */
    xr = xTaskCreate(battery_task, "battery", 2048, NULL, 1, NULL);
    if (xr != pdPASS) {
        ESP_LOGW(TAG, "Battery task skipped");
    }

    /* Set timezone — adjust TZ string for your locale */
    setenv("TZ", "PST8PDT", 1);
    tzset();

    ESP_LOGI(TAG, "=== All systems go ===");
    ESP_LOGI(TAG, "Free heap: %lu internal: %lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    /* Home screen is loaded by splash_screen done callback.
     * If in provisioning mode, the provisioning screen is already shown
     * by wifi_manager_init() and the splash callback won't override it. */
    if (wifi_manager_get_state() == WIFI_STATE_PROVISIONING) {
        ESP_LOGI(TAG, "In provisioning mode — provisioning screen stays active");
    }

    /* Configure side button (GPIO 0 / BOOT) as input with pull-up */
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);

    ESP_LOGI(TAG, "Button on GPIO %d ready — press to record", BUTTON_GPIO);

    /* Main loop: poll button + handle screen action bar button flags.
     * Physical button: any press while idle → start INGEST recording.
     *                  press while recording → stop.
     * LISTEN button (screen): same but sends to /query.
     * READ ALL (screen): reads current items via TTS.
     */
    bool btn_was_pressed   = false;
    bool btn_used_for_stop = false;
    static char s_rec_path[128] = {0};

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30));

        bool btn_now = (gpio_get_level(BUTTON_GPIO) == 0);  /* Active low */

        /* (LISTEN merged into VOICE — no separate handler needed) */

        /* --- Handle screen RECORD button flag --- */
        if (s_screen_record_pressed) {
            s_screen_record_pressed = false;
            if (audio_is_playing()) {
                /* Stop playback */
                audio_stop_playback();
                ui_set_playing(false);
            } else if (audio_is_recording()) {
                goto stop_recording;
            } else if (!s_uploading) {
                /* Start VOICE recording (unified) */
                s_listen_mode = true;
                const char *path = audio_start_recording();
                if (path) {
                    strlcpy(s_rec_path, path, sizeof(s_rec_path));
                    ui_set_recording(true);
                    ui_set_recording_hint("Tap to stop");
                    ui_show_screen(UI_SCREEN_RECORDING);
                    ESP_LOGI(TAG, "VOICE recording to: %s", s_rec_path);
                }
            }
        }

        /* --- Handle READ ALL button flag --- */
        if (s_screen_read_all_pressed) {
            s_screen_read_all_pressed = false;
            /* VIEW ALL: show the All Items (Today) screen */
            ui_show_screen(UI_SCREEN_TODAY);
        }

        /* --- Physical button press --- */
        if (btn_now && !btn_was_pressed) {
            bool was_asleep = !display_is_on();
            display_activity_signal();

            if (was_asleep) {
                btn_used_for_stop = true;
                ESP_LOGI(TAG, "Button press consumed for display wake");
            } else if (audio_is_playing()) {
                /* Stop audio playback */
                audio_stop_playback();
                btn_used_for_stop = true;
                ui_set_playing(false);
                ESP_LOGI(TAG, "Playback stopped by button");
            } else if (audio_is_recording()) {
                stop_recording:
                audio_stop_recording();
                btn_used_for_stop = true;
                ui_set_recording(false);
                ESP_LOGI(TAG, "Recording stopped (mode=%s)",
                         s_listen_mode ? "LISTEN" : "INGEST");

                if (strlen(s_rec_path) > 0) {
                    sd_queue_add(s_rec_path);
                }

                ui_set_processing_mode(s_listen_mode);
                ui_show_screen(UI_SCREEN_PROCESSING);

                /* All voice goes through /voice endpoint (0x02 = query/voice path) */
                if (s_upload_task_handle) {
                    xTaskNotify(s_upload_task_handle, 0x02, eSetBits);
                } else {
                    do_query();
                }

                s_rec_path[0] = '\0';
                s_listen_mode = false;
            } else {
                btn_used_for_stop = false;
            }
        }

        /* --- Physical button release — start INGEST recording --- */
        if (!btn_now && btn_was_pressed) {
            if (btn_used_for_stop) {
                btn_used_for_stop = false;
            } else if (!audio_is_recording() && !s_uploading && !audio_is_playing()) {
                /* Physical button always starts VOICE recording */
                s_listen_mode = true;  /* /voice handles both save and query */
                const char *path = audio_start_recording();
                if (path) {
                    strlcpy(s_rec_path, path, sizeof(s_rec_path));
                    ui_set_recording(true);
                    ui_set_recording_hint("Tap to stop");
                    ui_show_screen(UI_SCREEN_RECORDING);
                    ESP_LOGI(TAG, "Physical button VOICE recording: %s", s_rec_path);
                } else {
                    ESP_LOGW(TAG, "Failed to start recording");
                }
            }
        }

        btn_was_pressed = btn_now;
    }
}
