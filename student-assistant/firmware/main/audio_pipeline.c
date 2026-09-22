/**
 * Audio pipeline — wraps audio_capture module (BSP + esp_codec_dev)
 * for the Student Assistant watch.
 *
 * Recording: button-controlled via main.c.
 * Playback: MP3 decode via minimp3 → esp_codec_dev speaker output.
 * No VAD — recording is button-only.
 */
#include "audio_pipeline.h"
#include "audio_capture.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_codec_dev.h"

/* minimp3 — header-only MP3 decoder */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_STDIO
#include "minimp3.h"

static const char *TAG = "audio";

static volatile bool s_playback_stop = false;
static volatile bool s_is_playing    = false;

/* Recording state */
static volatile bool s_recording = false;
static char s_wav_path[64] = {0};
static uint32_t s_rec_counter = 0;
static TaskHandle_t s_rec_task_handle = NULL;

/* ---------- Recording task ----------
 * Runs audio_capture_record_to_wav() in a dedicated task.
 * Blocks until stop is requested or max duration reached.
 */
static void recording_task(void *arg)
{
    const char *path = (const char *)arg;
    audio_capture_config_t cfg = audio_capture_default_config();

    uint32_t pcm_bytes = 0;
    esp_err_t err = audio_capture_record_to_wav(path, &cfg, &pcm_bytes);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Recording failed: %s (err=%d)", path, err);
    } else {
        ESP_LOGI(TAG, "Recording saved: %s (%lu bytes)", path, (unsigned long)pcm_bytes);
    }

    s_recording = false;
    s_rec_task_handle = NULL;
    vTaskDelete(NULL);
}

/* ---------- Public API ---------- */

esp_err_t audio_pipeline_init(void)
{
    ESP_LOGI(TAG, "Initializing audio pipeline (BSP + esp_codec_dev)...");

    esp_err_t err = audio_capture_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio_capture_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Audio pipeline initialized (no VAD — button-controlled recording)");
    return ESP_OK;
}

const char *audio_start_recording(void)
{
    if (s_recording) return s_wav_path;

    /* Generate path on SD card */
    snprintf(s_wav_path, sizeof(s_wav_path), "/sdcard/queue/rec_%04lu.wav",
             (unsigned long)(s_rec_counter++));

    s_recording = true;

    /* Launch recording task (blocking call inside) */
    BaseType_t xr = xTaskCreate(recording_task, "rec",
                                 8192, (void *)s_wav_path,
                                 5, &s_rec_task_handle);
    if (xr != pdPASS) {
        ESP_LOGE(TAG, "Failed to create recording task");
        s_recording = false;
        return NULL;
    }

    ESP_LOGI(TAG, "Recording started → %s", s_wav_path);
    return s_wav_path;
}

void audio_stop_recording(void)
{
    if (!s_recording) return;

    /* Signal the blocking record function to stop */
    audio_capture_request_stop();
    ESP_LOGI(TAG, "Recording stop requested");

    /* Wait briefly for the task to finish (it should stop within ~1 chunk read) */
    for (int i = 0; i < 20 && s_recording; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (s_recording) {
        ESP_LOGW(TAG, "Recording task still running after 1s — continuing anyway");
        s_recording = false;
    }
}

bool audio_is_recording(void)
{
    return s_recording;
}

uint16_t audio_get_rms(void)
{
    return audio_capture_get_rms();
}

esp_err_t audio_play_mp3(const uint8_t *data, size_t len)
{
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;

    esp_codec_dev_handle_t spk = (esp_codec_dev_handle_t)audio_capture_get_speaker_dev();
    if (!spk) {
        ESP_LOGW(TAG, "No speaker device — playback skipped");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "MP3 playback: %d bytes", (int)len);

    /* Decode MP3 → PCM using minimp3 */
    static mp3dec_t dec;
    mp3dec_init(&dec);

    mp3dec_frame_info_t info;
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int offset = 0;
    bool codec_opened = false;

    while (offset < (int)len) {
        int samples = mp3dec_decode_frame(&dec, data + offset,
                                           len - offset, pcm, &info);
        if (info.frame_bytes == 0) break;
        offset += info.frame_bytes;

        if (samples > 0) {
            /* Open codec on first valid frame (so we know sample rate/channels) */
            if (!codec_opened) {
                esp_codec_dev_sample_info_t fs = {
                    .bits_per_sample = 16,
                    .channel = info.channels,
                    .channel_mask = 0,
                    .sample_rate = info.hz,
                    .mclk_multiple = 0,
                };
                if (esp_codec_dev_open(spk, &fs) != ESP_CODEC_DEV_OK) {
                    ESP_LOGE(TAG, "Failed to open speaker codec");
                    return ESP_FAIL;
                }
                esp_codec_dev_set_out_vol(spk, 70);  /* 70% volume */
                codec_opened = true;
                ESP_LOGI(TAG, "Speaker opened: %d Hz, %d ch", info.hz, info.channels);
            }

            /* Downmix stereo to mono if needed */
            int16_t *out = pcm;
            int out_bytes = samples * info.channels * sizeof(int16_t);

            int16_t mono_buf[MINIMP3_MAX_SAMPLES_PER_FRAME];
            if (info.channels == 2) {
                for (int i = 0; i < samples; i++) {
                    mono_buf[i] = (int16_t)(((int32_t)pcm[i * 2] + pcm[i * 2 + 1]) / 2);
                }
                out = mono_buf;
                out_bytes = samples * sizeof(int16_t);
            }

            esp_codec_dev_write(spk, out, out_bytes);
        }
    }

    if (codec_opened) {
        /* Brief silence to flush DAC */
        int16_t silence[256] = {0};
        esp_codec_dev_write(spk, silence, sizeof(silence));
        esp_codec_dev_close(spk);
    }

    ESP_LOGI(TAG, "MP3 playback done (decoded %d bytes)", offset);
    return ESP_OK;
}

esp_err_t audio_play_wav(const uint8_t *data, size_t len)
{
    if (!data || len <= 44) return ESP_ERR_INVALID_ARG;

    esp_codec_dev_handle_t spk = (esp_codec_dev_handle_t)audio_capture_get_speaker_dev();
    if (!spk) {
        ESP_LOGW(TAG, "No speaker device — playback skipped");
        return ESP_ERR_NOT_FOUND;
    }

    /* Parse WAV header for sample rate and channels */
    uint16_t channels = data[22] | (data[23] << 8);
    uint32_t sample_rate = data[24] | (data[25] << 8) | (data[26] << 16) | (data[27] << 24);
    uint16_t bits = data[34] | (data[35] << 8);

    /* Find "data" chunk */
    size_t pcm_offset = 44;  /* Standard WAV header */
    for (size_t i = 12; i < len - 8; i++) {
        if (data[i] == 'd' && data[i+1] == 'a' && data[i+2] == 't' && data[i+3] == 'a') {
            pcm_offset = i + 8;  /* Skip "data" + 4-byte size */
            break;
        }
    }

    size_t pcm_len = len - pcm_offset;
    ESP_LOGI(TAG, "WAV playback: %u bytes PCM (%lu Hz, %d ch, %d bit)",
             (unsigned)pcm_len, (unsigned long)sample_rate, channels, bits);

    /* Open speaker codec */
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = bits,
        .channel = channels,
        .channel_mask = 0,
        .sample_rate = sample_rate,
        .mclk_multiple = 0,
    };
    if (esp_codec_dev_open(spk, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Failed to open speaker codec");
        return ESP_FAIL;
    }
    esp_codec_dev_set_out_vol(spk, 80);

    /* Write PCM data in chunks */
    const size_t chunk_size = 2048;
    const uint8_t *pcm = data + pcm_offset;
    size_t written = 0;
    s_is_playing = true;
    s_playback_stop = false;
    while (written < pcm_len && !s_playback_stop) {
        size_t to_write = pcm_len - written;
        if (to_write > chunk_size) to_write = chunk_size;
        esp_codec_dev_write(spk, (void *)(pcm + written), (int)to_write);
        written += to_write;
    }
    s_is_playing = false;

    /* Flush with silence and close */
    int16_t silence[256] = {0};
    esp_codec_dev_write(spk, silence, sizeof(silence));
    esp_codec_dev_close(spk);

    ESP_LOGI(TAG, "WAV playback done (%u bytes)", (unsigned)pcm_len);
    return ESP_OK;
}

/* ---------- Streaming WAV playback ---------- */

esp_err_t audio_play_wav_stream_open(wav_stream_ctx_t *ctx)
{
    if (!ctx) return ESP_ERR_INVALID_ARG;
    memset(ctx, 0, sizeof(*ctx));
    ctx->spk = audio_capture_get_speaker_dev();
    if (!ctx->spk) {
        ESP_LOGW(TAG, "No speaker device — streaming playback skipped");
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t audio_play_wav_stream_write(wav_stream_ctx_t *ctx, const uint8_t *data, size_t len)
{
    if (!ctx || !ctx->spk || !data || len == 0) return ESP_ERR_INVALID_ARG;

    const uint8_t *pcm = data;
    size_t pcm_len = len;

    /* First chunk: parse WAV header and open codec */
    if (!ctx->opened) {
        if (len < 44) {
            ESP_LOGW(TAG, "Stream write: chunk too small for WAV header (%u bytes)", (unsigned)len);
            return ESP_ERR_INVALID_SIZE;
        }

        uint16_t channels = data[22] | (data[23] << 8);
        uint32_t sample_rate = data[24] | (data[25] << 8) | (data[26] << 16) | (data[27] << 24);
        uint16_t bits = data[34] | (data[35] << 8);

        /* Find "data" chunk */
        ctx->header_size = 44;
        for (size_t i = 12; i < len - 8; i++) {
            if (data[i] == 'd' && data[i+1] == 'a' && data[i+2] == 't' && data[i+3] == 'a') {
                ctx->header_size = i + 8;
                break;
            }
        }

        ESP_LOGI(TAG, "WAV stream: %lu Hz, %d ch, %d bit, header=%u",
                 (unsigned long)sample_rate, channels, bits, (unsigned)ctx->header_size);

        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = bits,
            .channel = channels,
            .channel_mask = 0,
            .sample_rate = sample_rate,
            .mclk_multiple = 0,
        };
        esp_codec_dev_handle_t spk = (esp_codec_dev_handle_t)ctx->spk;
        if (esp_codec_dev_open(spk, &fs) != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "Failed to open speaker codec for streaming");
            return ESP_FAIL;
        }
        esp_codec_dev_set_out_vol(spk, 80);
        ctx->opened = true;

        /* Skip header in first chunk */
        pcm = data + ctx->header_size;
        pcm_len = len - ctx->header_size;
    }

    /* Write PCM data directly to speaker */
    if (pcm_len > 0) {
        esp_codec_dev_write((esp_codec_dev_handle_t)ctx->spk, (void *)pcm, (int)pcm_len);
        ctx->total_pcm += pcm_len;
    }

    return ESP_OK;
}

esp_err_t audio_play_wav_stream_close(wav_stream_ctx_t *ctx)
{
    if (!ctx) return ESP_ERR_INVALID_ARG;
    if (ctx->opened && ctx->spk) {
        int16_t silence[256] = {0};
        esp_codec_dev_write((esp_codec_dev_handle_t)ctx->spk, silence, sizeof(silence));
        esp_codec_dev_close((esp_codec_dev_handle_t)ctx->spk);
        ESP_LOGI(TAG, "WAV stream done (%u bytes PCM)", (unsigned)ctx->total_pcm);
    }
    memset(ctx, 0, sizeof(*ctx));
    return ESP_OK;
}

void audio_set_volume(uint8_t vol)
{
    esp_codec_dev_handle_t spk = (esp_codec_dev_handle_t)audio_capture_get_speaker_dev();
    if (spk) {
        esp_codec_dev_set_out_vol(spk, (int)vol);
        ESP_LOGI(TAG, "Volume set to %d", vol);
    }
}

void audio_stop_playback(void)
{
    s_playback_stop = true;
    ESP_LOGI(TAG, "Playback stop requested");
}

bool audio_is_playing(void)
{
    return s_is_playing;
}
