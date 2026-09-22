#include "api_client.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/md.h"
#include "cJSON.h"

static const char *TAG = "api";

#define IO_CHUNK_SIZE  2048   /* Stream file in 2KB chunks (matches reference) */

static char s_device_id[32] = {0};
static char s_server_url[128] = {0};
static char s_token[65] = {0};

/* HMAC-SHA256(secret, device_id) */
static void compute_hmac(const char *secret, const char *device_id, char *out)
{
    uint8_t hash[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&ctx, (const uint8_t *)secret, strlen(secret));
    mbedtls_md_hmac_update(&ctx, (const uint8_t *)device_id, strlen(device_id));
    mbedtls_md_hmac_finish(&ctx, hash);
    mbedtls_md_free(&ctx);

    for (int i = 0; i < 32; i++) {
        sprintf(out + i * 2, "%02x", hash[i]);
    }
    out[64] = '\0';
}

/* Get file size without loading it */
static size_t get_file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (size_t)st.st_size;
}

/* Multipart boundary and helpers */
static const char *BOUNDARY = "----SA_Boundary_2024";

static int build_multipart_header(char *buf, size_t buf_sz)
{
    return snprintf(buf, buf_sz,
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"audio\"; filename=\"recording.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n",
        BOUNDARY);
}

static int build_multipart_footer(char *buf, size_t buf_sz)
{
    return snprintf(buf, buf_sz, "\r\n--%s--\r\n", BOUNDARY);
}

/* ---------- Public API ---------- */

esp_err_t api_client_init(const char *device_id, const char *server_url)
{
    strlcpy(s_device_id, device_id, sizeof(s_device_id));
    strlcpy(s_server_url, server_url, sizeof(s_server_url));

    const char *secret = "change-me-32-chars";
    compute_hmac(secret, device_id, s_token);

    ESP_LOGI(TAG, "API client init: device=%s server=%s", device_id, server_url);
    return ESP_OK;
}

/*
 * Streaming multipart upload: reads WAV from SD in 2KB chunks.
 * Uses esp_http_client_open/write/fetch_headers/read instead of perform.
 * This avoids loading the entire file into RAM.
 */
esp_err_t api_ingest_audio(const char *wav_path, const char *domain, ingest_result_t *result)
{
    memset(result, 0, sizeof(*result));
    esp_err_t ret = ESP_FAIL;

    /* Get file size */
    size_t wav_len = get_file_size(wav_path);
    if (wav_len == 0) {
        ESP_LOGE(TAG, "Cannot stat %s", wav_path);
        strcpy(result->status, "error");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "WAV file: %s (%u bytes)", wav_path, (unsigned)wav_len);

    /* Skip files that are just a WAV header (44 bytes) with no actual audio */
    if (wav_len <= 100) {
        ESP_LOGW(TAG, "WAV file too small (%u bytes), skipping", (unsigned)wav_len);
        strcpy(result->status, "error");
        return ESP_FAIL;
    }

    /* Calculate total content length for multipart */
    char mp_header[256];
    int mp_hdr_len = build_multipart_header(mp_header, sizeof(mp_header));
    char mp_footer[64];
    int mp_ftr_len = build_multipart_footer(mp_footer, sizeof(mp_footer));
    size_t total_len = mp_hdr_len + wav_len + mp_ftr_len;

    /* Build content-type header */
    char content_type[128];
    snprintf(content_type, sizeof(content_type),
             "multipart/form-data; boundary=%s", BOUNDARY);

    /* Build URL and timestamp */
    char url[192];
    snprintf(url, sizeof(url), "%s/ingest", s_server_url);
    char ts[16];
    snprintf(ts, sizeof(ts), "%ld", (long)time(NULL));

    ESP_LOGI(TAG, "POST %s (streaming %u bytes)", url, (unsigned)total_len);

    /* Create HTTP client */
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 90000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
        .buffer_size = 1024,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "esp_http_client_init FAILED");
        strcpy(result->status, "error");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_header(client, "x-device-id", s_device_id);
    esp_http_client_set_header(client, "x-device-token", s_token);
    esp_http_client_set_header(client, "x-device-ts", ts);
    esp_http_client_set_header(client, "x-domain", domain ? domain : "general");

    /* Open connection with known content length */
    ESP_LOGI(TAG, "Opening HTTPS connection...");
    esp_err_t err = esp_http_client_open(client, (int)total_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        goto cleanup;
    }
    ESP_LOGI(TAG, "Connection open, streaming data...");

    /* Write multipart header */
    int written = esp_http_client_write(client, mp_header, mp_hdr_len);
    if (written < 0) {
        ESP_LOGE(TAG, "Failed to write multipart header");
        goto cleanup;
    }

    /* Stream WAV file in chunks */
    {
        FILE *f = fopen(wav_path, "rb");
        if (!f) {
            ESP_LOGE(TAG, "Cannot open %s for reading", wav_path);
            goto cleanup;
        }

        char chunk[IO_CHUNK_SIZE];
        size_t total_sent = 0;
        while (total_sent < wav_len) {
            size_t to_read = wav_len - total_sent;
            if (to_read > IO_CHUNK_SIZE) to_read = IO_CHUNK_SIZE;

            size_t got = fread(chunk, 1, to_read, f);
            if (got == 0) {
                ESP_LOGE(TAG, "File read error at offset %u", (unsigned)total_sent);
                fclose(f);
                goto cleanup;
            }

            written = esp_http_client_write(client, chunk, (int)got);
            if (written < 0) {
                ESP_LOGE(TAG, "HTTP write failed at offset %u", (unsigned)total_sent);
                fclose(f);
                goto cleanup;
            }
            total_sent += got;

            /* Log progress every ~100KB */
            if ((total_sent % (100 * 1024)) < IO_CHUNK_SIZE) {
                ESP_LOGI(TAG, "Sent %u / %u bytes (%.0f%%)",
                         (unsigned)total_sent, (unsigned)wav_len,
                         100.0f * total_sent / wav_len);
            }
        }
        fclose(f);
        ESP_LOGI(TAG, "WAV data sent: %u bytes", (unsigned)total_sent);
    }

    /* Write multipart footer */
    written = esp_http_client_write(client, mp_footer, mp_ftr_len);
    if (written < 0) {
        ESP_LOGE(TAG, "Failed to write multipart footer");
        goto cleanup;
    }

    /* Fetch response headers */
    ESP_LOGI(TAG, "Waiting for server response...");
    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Response: status=%d content_length=%d", status_code, content_length);

    if (status_code != 200) {
        ESP_LOGE(TAG, "Ingest failed: status=%d", status_code);
        goto cleanup;
    }

    /* Read response body */
    {
        char resp_buf[3072] = {0};
        int resp_len = 0;
        int read_len;
        while ((read_len = esp_http_client_read(client, resp_buf + resp_len,
                                                 sizeof(resp_buf) - resp_len - 1)) > 0) {
            resp_len += read_len;
            if (resp_len >= (int)sizeof(resp_buf) - 1) break;
        }
        resp_buf[resp_len] = '\0';
        ESP_LOGI(TAG, "Response body: %s", resp_buf);

        /* Parse JSON */
        cJSON *json = cJSON_Parse(resp_buf);
        if (!json) {
            ESP_LOGE(TAG, "JSON parse failed");
            goto cleanup;
        }

        cJSON *j;
        if ((j = cJSON_GetObjectItem(json, "status")) && cJSON_IsString(j))
            strlcpy(result->status, j->valuestring, sizeof(result->status));
        if ((j = cJSON_GetObjectItem(json, "session_id")) && cJSON_IsString(j))
            strlcpy(result->session_id, j->valuestring, sizeof(result->session_id));
        if ((j = cJSON_GetObjectItem(json, "domain")) && cJSON_IsString(j))
            strlcpy(result->domain, j->valuestring, sizeof(result->domain));

        /* Parse items array (new multi-item format) */
        cJSON *items_arr = cJSON_GetObjectItem(json, "items");
        if (items_arr && cJSON_IsArray(items_arr)) {
            int n = cJSON_GetArraySize(items_arr);
            if (n > MAX_INGEST_ITEMS) n = MAX_INGEST_ITEMS;
            result->count = n;
            for (int i = 0; i < n; i++) {
                cJSON *item = cJSON_GetArrayItem(items_arr, i);
                ingest_item_t *r = &result->items[i];
                if ((j = cJSON_GetObjectItem(item, "item_id")) && cJSON_IsString(j))
                    strlcpy(r->item_id, j->valuestring, sizeof(r->item_id));
                if ((j = cJSON_GetObjectItem(item, "type")) && cJSON_IsString(j))
                    strlcpy(r->type, j->valuestring, sizeof(r->type));
                if ((j = cJSON_GetObjectItem(item, "category")) && cJSON_IsString(j))
                    strlcpy(r->category, j->valuestring, sizeof(r->category));
                if ((j = cJSON_GetObjectItem(item, "title")) && cJSON_IsString(j))
                    strlcpy(r->title, j->valuestring, sizeof(r->title));
                if ((j = cJSON_GetObjectItem(item, "due_date")) && cJSON_IsString(j))
                    strlcpy(r->due_date, j->valuestring, sizeof(r->due_date));
                if ((j = cJSON_GetObjectItem(item, "datetime")) && cJSON_IsString(j))
                    strlcpy(r->datetime, j->valuestring, sizeof(r->datetime));
                if ((j = cJSON_GetObjectItem(item, "recurrence")) && cJSON_IsString(j))
                    strlcpy(r->recurrence, j->valuestring, sizeof(r->recurrence));
                if ((j = cJSON_GetObjectItem(item, "confidence")) && cJSON_IsNumber(j))
                    r->confidence = (float)j->valuedouble;
            }
        } else {
            /* Backward compat: old backend without items array — read top-level fields */
            result->count = 1;
            ingest_item_t *r = &result->items[0];
            if ((j = cJSON_GetObjectItem(json, "item_id")) && cJSON_IsString(j))
                strlcpy(r->item_id, j->valuestring, sizeof(r->item_id));
            if ((j = cJSON_GetObjectItem(json, "type")) && cJSON_IsString(j))
                strlcpy(r->type, j->valuestring, sizeof(r->type));
            if ((j = cJSON_GetObjectItem(json, "title")) && cJSON_IsString(j))
                strlcpy(r->title, j->valuestring, sizeof(r->title));
            if ((j = cJSON_GetObjectItem(json, "due_date")) && cJSON_IsString(j))
                strlcpy(r->due_date, j->valuestring, sizeof(r->due_date));
            if ((j = cJSON_GetObjectItem(json, "datetime")) && cJSON_IsString(j))
                strlcpy(r->datetime, j->valuestring, sizeof(r->datetime));
            if ((j = cJSON_GetObjectItem(json, "confidence")) && cJSON_IsNumber(j))
                r->confidence = (float)j->valuedouble;
        }

        cJSON_Delete(json);
        for (int i = 0; i < result->count; i++) {
            ESP_LOGI(TAG, "Ingest OK [%d/%d]: status=%s type=%s title=%s conf=%.0f%%",
                     i + 1, result->count, result->status,
                     result->items[i].type, result->items[i].title,
                     result->items[i].confidence * 100);
        }
        ret = ESP_OK;
    }

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (ret != ESP_OK) {
        strcpy(result->status, "error");
    }
    return ret;
}

esp_err_t api_query_audio(const char *wav_path, audio_chunk_cb_t cb, void *ctx,
                          char *narration_out, size_t narration_size)
{
    /* Get file size */
    size_t wav_len = get_file_size(wav_path);
    if (wav_len == 0) {
        ESP_LOGE(TAG, "Cannot stat %s", wav_path);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Query: %s (%u bytes)", wav_path, (unsigned)wav_len);

    /* Calculate content length */
    char mp_header[256];
    int mp_hdr_len = build_multipart_header(mp_header, sizeof(mp_header));
    char mp_footer[64];
    int mp_ftr_len = build_multipart_footer(mp_footer, sizeof(mp_footer));
    size_t total_len = mp_hdr_len + wav_len + mp_ftr_len;

    char content_type[128];
    snprintf(content_type, sizeof(content_type),
             "multipart/form-data; boundary=%s", BOUNDARY);

    char url[192];
    snprintf(url, sizeof(url), "%s/query", s_server_url);
    char ts[16];
    snprintf(ts, sizeof(ts), "%ld", (long)time(NULL));

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 90000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
        .buffer_size = 1024,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "esp_http_client_init FAILED for query");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_header(client, "x-device-id", s_device_id);
    esp_http_client_set_header(client, "x-device-token", s_token);
    esp_http_client_set_header(client, "x-device-ts", ts);

    /* Open with content length */
    esp_err_t err = esp_http_client_open(client, (int)total_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Query HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    /* Write multipart header */
    esp_http_client_write(client, mp_header, mp_hdr_len);

    /* Stream WAV file */
    FILE *f = fopen(wav_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s", wav_path);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    char chunk[IO_CHUNK_SIZE];
    size_t total_sent = 0;
    while (total_sent < wav_len) {
        size_t to_read = wav_len - total_sent;
        if (to_read > IO_CHUNK_SIZE) to_read = IO_CHUNK_SIZE;
        size_t got = fread(chunk, 1, to_read, f);
        if (got == 0) break;
        if (esp_http_client_write(client, chunk, (int)got) < 0) break;
        total_sent += got;
    }
    fclose(f);

    /* Write footer */
    esp_http_client_write(client, mp_footer, mp_ftr_len);

    /* Read response (MP3 audio) */
    ESP_LOGI(TAG, "Query: waiting for response...");
    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Query: status=%d content_length=%d", status, content_length);

    /* Note: esp_http_client_get_header only works for request headers,
     * not response headers. We pass narration via a separate mechanism
     * (the backend also logs it). For now, narration stays empty on device. */
    if (narration_out && narration_size > 0) {
        narration_out[0] = '\0';
    }

    /* Stream response in chunks directly to callback (no large temp buffer) */
    if (cb) {
        char chunk[2048];
        int total_read = 0;
        int read_len;
        while ((read_len = esp_http_client_read(client, chunk, sizeof(chunk))) > 0) {
            esp_err_t cb_err = cb((const uint8_t *)chunk, read_len, ctx);
            total_read += read_len;
            if (cb_err != ESP_OK) {
                ESP_LOGW(TAG, "Query: callback aborted at %d bytes", total_read);
                break;
            }
        }
        ESP_LOGI(TAG, "Query: streamed %d bytes to callback", total_read);
    } else {
        /* No callback — just drain the response */
        char drain[512];
        while (esp_http_client_read(client, drain, sizeof(drain)) > 0) {}
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_OK;
}


/* ---------- Unified /voice endpoint ---------- */

esp_err_t api_voice_audio(const char *wav_path, const char *domain,
                           audio_chunk_cb_t cb, void *ctx,
                           voice_result_t *result)
{
    if (result) {
        memset(result, 0, sizeof(*result));
        strcpy(result->mode, "query");  /* default */
    }

    size_t wav_len = get_file_size(wav_path);
    if (wav_len == 0) {
        ESP_LOGE(TAG, "Voice: cannot stat %s", wav_path);
        return ESP_FAIL;
    }
    if (wav_len <= 100) {
        ESP_LOGW(TAG, "Voice: WAV too small, skipping");
        return ESP_FAIL;
    }

    char mp_header[256];
    int mp_hdr_len = build_multipart_header(mp_header, sizeof(mp_header));
    char mp_footer[64];
    int mp_ftr_len = build_multipart_footer(mp_footer, sizeof(mp_footer));
    size_t total_len = mp_hdr_len + wav_len + mp_ftr_len;

    char content_type[128];
    snprintf(content_type, sizeof(content_type),
             "multipart/form-data; boundary=%s", BOUNDARY);

    char url[192];
    snprintf(url, sizeof(url), "%s/voice", s_server_url);
    char ts[16];
    snprintf(ts, sizeof(ts), "%ld", (long)time(NULL));

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 90000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
        .buffer_size = 1024,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { ESP_LOGE(TAG, "Voice: client init failed"); return ESP_FAIL; }

    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_header(client, "x-device-id", s_device_id);
    esp_http_client_set_header(client, "x-device-token", s_token);
    esp_http_client_set_header(client, "x-device-ts", ts);
    esp_http_client_set_header(client, "x-domain", domain ? domain : "general");

    esp_err_t err = esp_http_client_open(client, (int)total_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Voice: open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    /* Write multipart header */
    esp_http_client_write(client, mp_header, mp_hdr_len);

    /* Stream WAV file */
    FILE *f = fopen(wav_path, "rb");
    if (!f) {
        esp_http_client_close(client); esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    char chunk[IO_CHUNK_SIZE];
    size_t sent = 0;
    while (sent < wav_len) {
        size_t to_read = wav_len - sent;
        if (to_read > IO_CHUNK_SIZE) to_read = IO_CHUNK_SIZE;
        size_t got = fread(chunk, 1, to_read, f);
        if (got == 0) break;
        if (esp_http_client_write(client, chunk, (int)got) < 0) break;
        sent += got;
    }
    fclose(f);

    /* Footer */
    esp_http_client_write(client, mp_footer, mp_ftr_len);

    /* Await response */
    ESP_LOGI(TAG, "Voice: waiting for response...");
    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Voice: status=%d len=%d", status, content_length);

    if (status != 200) {
        char drain[128];
        while (esp_http_client_read(client, drain, sizeof(drain)) > 0) {}
        esp_http_client_close(client); esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    /* Stream WAV response to callback */
    if (cb) {
        char rbuf[2048];
        int total_read = 0, rlen;
        while ((rlen = esp_http_client_read(client, rbuf, sizeof(rbuf))) > 0) {
            esp_err_t cberr = cb((const uint8_t *)rbuf, rlen, ctx);
            total_read += rlen;
            if (cberr != ESP_OK) {
                ESP_LOGW(TAG, "Voice: callback aborted at %d bytes", total_read);
                break;
            }
        }
        ESP_LOGI(TAG, "Voice: %d bytes streamed", total_read);
    } else {
        char drain[512];
        while (esp_http_client_read(client, drain, sizeof(drain)) > 0) {}
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_OK;
}

/* ---------- Fetch items from /items ---------- */

int api_fetch_items(api_item_t *out, int max_items)
{
    if (!out || max_items <= 0) return -1;

    char url[192];
    snprintf(url, sizeof(url), "%s/items/%s", s_server_url, s_device_id);

    char ts[16];
    snprintf(ts, sizeof(ts), "%ld", (long)time(NULL));

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size_tx = 512,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "x-device-id", s_device_id);
    esp_http_client_set_header(client, "x-device-token", s_token);
    esp_http_client_set_header(client, "x-device-ts", ts);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Items: connection failed (%d)", err);
        esp_http_client_cleanup(client);
        return -1;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Items: status=%d content_length=%d", status, content_length);

    if (status != 200) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }

    /* Read response */
    char *resp = malloc(4096);
    if (!resp) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }
    int resp_len = 0, read_len;
    while ((read_len = esp_http_client_read(client, resp + resp_len, 4095 - resp_len)) > 0) {
        resp_len += read_len;
        if (resp_len >= 4094) break;
    }
    resp[resp_len] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    /* Parse JSON array */
    cJSON *arr = cJSON_Parse(resp);
    free(resp);
    if (!arr || !cJSON_IsArray(arr)) {
        ESP_LOGW(TAG, "Items: invalid JSON");
        if (arr) cJSON_Delete(arr);
        return -1;
    }

    int count = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        if (count >= max_items) break;
        cJSON *j;
        memset(&out[count], 0, sizeof(api_item_t));
        if ((j = cJSON_GetObjectItem(item, "id")) && cJSON_IsString(j))
            strlcpy(out[count].id, j->valuestring, sizeof(out[count].id));
        if ((j = cJSON_GetObjectItem(item, "type")) && cJSON_IsString(j))
            strlcpy(out[count].type, j->valuestring, sizeof(out[count].type));
        if ((j = cJSON_GetObjectItem(item, "title")) && cJSON_IsString(j))
            strlcpy(out[count].title, j->valuestring, sizeof(out[count].title));
        if ((j = cJSON_GetObjectItem(item, "due_date")) && cJSON_IsString(j))
            strlcpy(out[count].due_date, j->valuestring, sizeof(out[count].due_date));
        if ((j = cJSON_GetObjectItem(item, "datetime")) && cJSON_IsString(j))
            strlcpy(out[count].datetime, j->valuestring, sizeof(out[count].datetime));
        if ((j = cJSON_GetObjectItem(item, "done")) && cJSON_IsNumber(j))
            out[count].done = (int)j->valuedouble;
        if ((j = cJSON_GetObjectItem(item, "paused")) && cJSON_IsNumber(j))
            out[count].paused = (int)j->valuedouble;
        count++;
    }
    cJSON_Delete(arr);
    ESP_LOGI(TAG, "Items: fetched %d items", count);
    return count;
}

/* ---------- Item actions (toggle/pause/delete) ---------- */

static esp_err_t item_action_request(const char *item_id, const char *endpoint, esp_http_client_method_t method)
{
    char url[256];
    snprintf(url, sizeof(url), "%s/items/%s%s", s_server_url, item_id, endpoint);

    char ts[16];
    snprintf(ts, sizeof(ts), "%ld", (long)time(NULL));

    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size_tx = 512,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "x-device-id", s_device_id);
    esp_http_client_set_header(client, "x-device-token", s_token);
    esp_http_client_set_header(client, "x-device-ts", ts);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGE(TAG, "Item action %s failed: err=%d status=%d", endpoint, err, status);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Item action %s/%s: status=%d", item_id, endpoint, status);
    return ESP_OK;
}

esp_err_t api_toggle_item(const char *item_id)
{
    return item_action_request(item_id, "/toggle", HTTP_METHOD_PATCH);
}

esp_err_t api_pause_item(const char *item_id)
{
    return item_action_request(item_id, "/pause", HTTP_METHOD_PATCH);
}

esp_err_t api_delete_item(const char *item_id)
{
    return item_action_request(item_id, "", HTTP_METHOD_DELETE);
}

/* ---------- TTS endpoint ---------- */

esp_err_t api_tts(const char *text, audio_chunk_cb_t cb, void *ctx)
{
    if (!text || !cb) return ESP_ERR_INVALID_ARG;

    char url[192];
    snprintf(url, sizeof(url), "%s/tts", s_server_url);

    /* Build JSON body: {"text": "..."} */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "text", text);
    char *json_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json_str) return ESP_ERR_NO_MEM;

    int json_len = strlen(json_str);
    ESP_LOGI(TAG, "TTS: '%s' (%d bytes JSON)", text, json_len);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(json_str);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "x-device-id", s_device_id);
    esp_http_client_set_header(client, "x-device-token", s_token);

    esp_err_t err = esp_http_client_open(client, json_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TTS: open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(json_str);
        return ESP_FAIL;
    }

    esp_http_client_write(client, json_str, json_len);
    free(json_str);

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "TTS: status=%d content_length=%d", status, content_length);

    if (status != 200) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    /* Stream WAV response in chunks to callback */
    char chunk[2048];
    int total_read = 0;
    int read_len;
    while ((read_len = esp_http_client_read(client, chunk, sizeof(chunk))) > 0) {
        esp_err_t cb_err = cb((const uint8_t *)chunk, read_len, ctx);
        total_read += read_len;
        if (cb_err != ESP_OK) break;
    }
    ESP_LOGI(TAG, "TTS: streamed %d bytes", total_read);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_OK;
}
