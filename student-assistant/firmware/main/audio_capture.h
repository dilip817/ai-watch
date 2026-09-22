#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t sample_rate;        // e.g. 16000
    uint16_t bits_per_sample;    // e.g. 16
    uint16_t channels;           // e.g. 1
    uint16_t chunk_bytes;        // e.g. 2048
    uint16_t record_gain_q8;     // 256 = 1.0x, 384 = 1.5x
    float mic_gain_db;           // e.g. 42.0f
    uint32_t max_duration_ms;    // 0 = unlimited
} audio_capture_config_t;

/**
 * Initialize mic codec via Waveshare BSP (esp_codec_dev).
 * Does NOT mount SPIFFS — we use SD card for storage.
 */
esp_err_t audio_capture_init(void);

/**
 * Returns a practical default config for watch recording.
 * 16kHz mono 16-bit, 42dB codec gain, 1.5x digital gain.
 */
audio_capture_config_t audio_capture_default_config(void);

/**
 * Request stop for current blocking recording call.
 * Safe to call from any task (uses volatile flag).
 */
void audio_capture_request_stop(void);

/**
 * Blocking WAV capture to the given path (can be SD card or SPIFFS).
 * Stops when request_stop() is called or max_duration_ms reached.
 * Returns ESP_OK on success with pcm byte count in out_pcm_bytes.
 */
esp_err_t audio_capture_record_to_wav(const char *wav_path,
                                       const audio_capture_config_t *cfg,
                                       uint32_t *out_pcm_bytes);

/**
 * Get current RMS level during recording (0–32767).
 * Updated each chunk read. Returns 0 when not recording.
 */
uint16_t audio_capture_get_rms(void);

/**
 * Get the BSP speaker codec device handle (for MP3 playback).
 * Returns NULL if speaker not initialized.
 */
void *audio_capture_get_speaker_dev(void);

#ifdef __cplusplus
}
#endif
