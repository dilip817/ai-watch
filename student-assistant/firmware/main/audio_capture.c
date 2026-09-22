/**
 * Audio capture module — adapted from codex_audio_capture_bundle.
 * Uses Waveshare BSP + esp_codec_dev for proper ES7210 codec handling.
 * Records to SD card (not SPIFFS) for our upload pipeline.
 */
#include "audio_capture.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <stdlib.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"

#define TAG "audio_cap"

static esp_codec_dev_handle_t s_mic_dev;
static esp_codec_dev_handle_t s_spk_dev;
static volatile bool s_stop_requested;
static volatile uint16_t s_current_rms;

/* ---------- WAV header helpers ---------- */

static void write_u16_le(uint8_t *dst, uint16_t v)
{
    dst[0] = (uint8_t)(v & 0xFF);
    dst[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void write_u32_le(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)(v & 0xFF);
    dst[1] = (uint8_t)((v >> 8) & 0xFF);
    dst[2] = (uint8_t)((v >> 16) & 0xFF);
    dst[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void write_wav_header(FILE *f, uint32_t pcm_bytes,
                              uint32_t sample_rate, uint16_t bits,
                              uint16_t channels)
{
    uint8_t h[44] = {0};
    uint32_t byte_rate = sample_rate * channels * (bits / 8);
    uint16_t block_align = channels * (bits / 8);

    memcpy(h + 0, "RIFF", 4);
    write_u32_le(h + 4, 36 + pcm_bytes);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    write_u32_le(h + 16, 16);
    write_u16_le(h + 20, 1);           /* PCM format */
    write_u16_le(h + 22, channels);
    write_u32_le(h + 24, sample_rate);
    write_u32_le(h + 28, byte_rate);
    write_u16_le(h + 32, block_align);
    write_u16_le(h + 34, bits);
    memcpy(h + 36, "data", 4);
    write_u32_le(h + 40, pcm_bytes);

    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), f);
    fflush(f);
}

/* ---------- Digital gain (Q8 fixed-point) ---------- */

static void apply_pcm_gain_i16(uint8_t *buf, size_t len, uint16_t gain_q8)
{
    if (!buf || len < 2) return;
    int16_t *samples = (int16_t *)buf;
    size_t count = len / 2;
    for (size_t i = 0; i < count; i++) {
        int32_t v = ((int32_t)samples[i] * (int32_t)gain_q8) / 256;
        if (v > 32767) v = 32767;
        else if (v < -32768) v = -32768;
        samples[i] = (int16_t)v;
    }
}

/* ---------- RMS calculation ---------- */

static uint16_t calc_rms_i16(const int16_t *samples, size_t count)
{
    if (count == 0) return 0;
    uint64_t sum = 0;
    for (size_t i = 0; i < count; i++) {
        int32_t s = samples[i];
        sum += (uint64_t)(s * s);
    }
    return (uint16_t)sqrtf((float)(sum / count));
}

/* ---------- Public API ---------- */

audio_capture_config_t audio_capture_default_config(void)
{
    audio_capture_config_t cfg = {
        .sample_rate     = 16000,
        .bits_per_sample = 16,
        .channels        = 1,
        .chunk_bytes     = 2048,
        .record_gain_q8  = 384,    /* 1.5x digital gain */
        .mic_gain_db     = 42.0f,  /* 42 dB codec gain (matches reference projects) */
        .max_duration_ms = 0,      /* unlimited */
    };
    return cfg;
}

esp_err_t audio_capture_init(void)
{
    /* Initialize microphone codec via BSP (handles I2C, I2S, ES7210 setup) */
    if (s_mic_dev == NULL) {
        s_mic_dev = bsp_audio_codec_microphone_init();
        if (s_mic_dev == NULL) {
            ESP_LOGE(TAG, "Microphone codec init failed (bsp_audio_codec_microphone_init)");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Microphone codec initialized via BSP");
    }

    /* Initialize speaker codec via BSP (for MP3 playback) */
    if (s_spk_dev == NULL) {
        s_spk_dev = bsp_audio_codec_speaker_init();
        if (s_spk_dev == NULL) {
            ESP_LOGW(TAG, "Speaker codec init failed (non-fatal, playback disabled)");
        } else {
            ESP_LOGI(TAG, "Speaker codec initialized via BSP");
        }
    }

    return ESP_OK;
}

void audio_capture_request_stop(void)
{
    s_stop_requested = true;
}

uint16_t audio_capture_get_rms(void)
{
    return s_current_rms;
}

void *audio_capture_get_speaker_dev(void)
{
    return (void *)s_spk_dev;
}

esp_err_t audio_capture_record_to_wav(const char *wav_path,
                                       const audio_capture_config_t *cfg_in,
                                       uint32_t *out_pcm_bytes)
{
    if (!wav_path || wav_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    audio_capture_config_t cfg = cfg_in ? *cfg_in : audio_capture_default_config();
    if (cfg.bits_per_sample != 16) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg.chunk_bytes < 256) {
        cfg.chunk_bytes = 2048;
    }

    /* Ensure mic is initialized */
    esp_err_t err = audio_capture_init();
    if (err != ESP_OK) {
        return err;
    }

    /* Open WAV file */
    FILE *f = fopen(wav_path, "wb+");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s errno=%d", wav_path, errno);
        return ESP_FAIL;
    }

    /* Write placeholder header (patched at end with correct size) */
    write_wav_header(f, 0, cfg.sample_rate, cfg.bits_per_sample, cfg.channels);

    /* Open codec device with desired sample format */
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = cfg.bits_per_sample,
        .channel         = cfg.channels,
        .channel_mask    = 0,
        .sample_rate     = cfg.sample_rate,
        .mclk_multiple   = 0,
    };
    if (esp_codec_dev_open(s_mic_dev, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed");
        fclose(f);
        remove(wav_path);
        return ESP_FAIL;
    }

    /* Set codec input gain (42 dB — much higher than our old 30 dB register writes) */
    esp_codec_dev_set_in_gain(s_mic_dev, cfg.mic_gain_db);
    ESP_LOGI(TAG, "Recording to %s (rate=%lu, gain=%.1fdB, digital=%.2fx)",
             wav_path, (unsigned long)cfg.sample_rate,
             cfg.mic_gain_db, (float)cfg.record_gain_q8 / 256.0f);

    /* Allocate read buffer */
    uint8_t *buf = (uint8_t *)malloc(cfg.chunk_bytes);
    if (!buf) {
        esp_codec_dev_close(s_mic_dev);
        fclose(f);
        remove(wav_path);
        return ESP_ERR_NO_MEM;
    }

    uint32_t pcm_bytes = 0;
    int read_fail_count = 0;
    int64_t start_us = esp_timer_get_time();
    s_stop_requested = false;
    s_current_rms = 0;

    while (true) {
        int ret = esp_codec_dev_read(s_mic_dev, buf, cfg.chunk_bytes);
        int captured_bytes = 0;

        if (ret == ESP_CODEC_DEV_OK) {
            captured_bytes = cfg.chunk_bytes;
        } else if (ret > 0) {
            captured_bytes = ret;
        }

        if (captured_bytes > 0) {
            /* Apply digital gain */
            apply_pcm_gain_i16(buf, (size_t)captured_bytes, cfg.record_gain_q8);

            /* Update RMS for waveform display */
            s_current_rms = calc_rms_i16((const int16_t *)buf,
                                          (size_t)captured_bytes / 2);

            /* Write PCM data to file */
            fwrite(buf, 1, (size_t)captured_bytes, f);
            pcm_bytes += (uint32_t)captured_bytes;
            read_fail_count = 0;

            /* Log first few chunks for diagnostics */
            if (pcm_bytes <= cfg.chunk_bytes * 3) {
                ESP_LOGI(TAG, "Chunk: %d bytes, RMS=%u, total=%lu",
                         captured_bytes, s_current_rms, (unsigned long)pcm_bytes);
            }
        } else {
            read_fail_count++;
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        /* Stop conditions */
        if (s_stop_requested && (pcm_bytes > 0)) {
            break;
        }
        if (s_stop_requested && (read_fail_count > 40)) {
            ESP_LOGW(TAG, "Stop requested but no data captured after 40 retries");
            break;
        }
        if (cfg.max_duration_ms > 0) {
            int64_t elapsed_ms = (esp_timer_get_time() - start_us) / 1000;
            if (elapsed_ms >= (int64_t)cfg.max_duration_ms) {
                break;
            }
        }
    }

    /* Close codec */
    esp_codec_dev_close(s_mic_dev);
    free(buf);
    s_current_rms = 0;

    if (pcm_bytes == 0) {
        ESP_LOGW(TAG, "No audio data captured, removing %s", wav_path);
        fclose(f);
        remove(wav_path);
        return ESP_FAIL;
    }

    /* Patch WAV header with final size */
    write_wav_header(f, pcm_bytes, cfg.sample_rate, cfg.bits_per_sample, cfg.channels);
    fclose(f);

    float duration_sec = (float)pcm_bytes / (cfg.sample_rate * cfg.channels * (cfg.bits_per_sample / 8));
    ESP_LOGI(TAG, "Recording complete: %lu bytes (%.1f sec)", (unsigned long)pcm_bytes, duration_sec);

    if (out_pcm_bytes) {
        *out_pcm_bytes = pcm_bytes;
    }
    return ESP_OK;
}
