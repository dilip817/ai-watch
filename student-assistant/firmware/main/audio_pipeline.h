#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/**
 * Initialize I2S for microphone (ES7210) and speaker (ES8311).
 * Pin assignments from Waveshare ESP32-S3 2.06" AMOLED factory firmware:
 *   MCLK=GPIO16, BCLK=GPIO41, WS=GPIO45, DIN=GPIO40, DOUT=GPIO42, PA=GPIO46
 * Audio codecs: ES8311 (DAC) + ES7210 (ADC) on I2C (SDA=GPIO15, SCL=GPIO14)
 */
esp_err_t audio_pipeline_init(void);

/**
 * Start recording to SD card. Creates WAV file at /sdcard/queue/rec_XXXX.wav
 * Returns the filename (caller must not free — static buffer).
 */
const char *audio_start_recording(void);

/**
 * Stop recording. Finalizes WAV header, closes file.
 */
void audio_stop_recording(void);

/**
 * Check if currently recording.
 */
bool audio_is_recording(void);

/**
 * Get current RMS level (0–32767) for waveform display.
 */
uint16_t audio_get_rms(void);

/**
 * Play WAV audio (16kHz, 16-bit, mono PCM) from memory buffer through the speaker.
 * Skips the 44-byte WAV header automatically. Blocks until playback completes or stopped.
 */
esp_err_t audio_play_wav(const uint8_t *data, size_t len);

/**
 * Stop any in-progress audio_play_wav() playback.
 * Safe to call from any task or LVGL callback.
 * audio_play_wav() will return ESP_ERR_INVALID_STATE when interrupted.
 */
void audio_stop_playback(void);

/**
 * Returns true if audio_play_wav() is currently running.
 */
bool audio_is_playing(void);

/**
 * Streaming WAV playback — feed chunks as they arrive from HTTP.
 * Call open() once, then write() for each chunk, then close().
 * The first write() must contain at least the 44-byte WAV header.
 */
typedef struct {
    void *spk;           /* opaque speaker handle */
    size_t header_size;  /* bytes consumed by WAV header */
    bool opened;         /* codec opened successfully */
    size_t total_pcm;    /* total PCM bytes written */
} wav_stream_ctx_t;

esp_err_t audio_play_wav_stream_open(wav_stream_ctx_t *ctx);
esp_err_t audio_play_wav_stream_write(wav_stream_ctx_t *ctx, const uint8_t *data, size_t len);
esp_err_t audio_play_wav_stream_close(wav_stream_ctx_t *ctx);

/**
 * Set speaker volume (0–100).
 */
void audio_set_volume(uint8_t vol);
