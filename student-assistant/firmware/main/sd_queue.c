#include "sd_queue.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "cJSON.h"

static const char *TAG = "sd_queue";

#define MOUNT_POINT    "/sdcard"
#define QUEUE_DIR      MOUNT_POINT "/queue"
#define FAILED_DIR     MOUNT_POINT "/failed"
#define MANIFEST_PATH  QUEUE_DIR "/manifest.json"
#define MANIFEST_TMP   QUEUE_DIR "/manifest.json.tmp"
#define MAX_RETRIES    5

/*
 * SD card pins from Waveshare factory firmware:
 *   CLK=GPIO2, CMD=GPIO1, DATA0=GPIO3
 * Using 1-line SDMMC mode.
 */
#define SD_CLK   GPIO_NUM_2
#define SD_CMD   GPIO_NUM_1
#define SD_DATA0 GPIO_NUM_3

static sdmmc_card_t *s_card = NULL;

/* ---------- Manifest helpers ---------- */

static cJSON *load_manifest(void)
{
    FILE *f = fopen(MANIFEST_PATH, "r");
    if (!f) {
        /* Create default manifest */
        cJSON *root = cJSON_CreateObject();
        cJSON_AddArrayToObject(root, "pending");
        return root;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        root = cJSON_CreateObject();
        cJSON_AddArrayToObject(root, "pending");
    }
    return root;
}

/* Atomic write: write .tmp, then rename */
static esp_err_t save_manifest(cJSON *root)
{
    char *str = cJSON_PrintUnformatted(root);
    if (!str) return ESP_ERR_NO_MEM;

    FILE *f = fopen(MANIFEST_TMP, "w");
    if (!f) { free(str); return ESP_FAIL; }
    fputs(str, f);
    fclose(f);
    free(str);

    /* Atomic rename */
    remove(MANIFEST_PATH);
    if (rename(MANIFEST_TMP, MANIFEST_PATH) != 0) {
        ESP_LOGE(TAG, "rename failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ---------- Public API ---------- */

esp_err_t sd_queue_init(void)
{
    ESP_LOGI(TAG, "Mounting SD card (SDMMC 1-line)");

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    host.flags = SDMMC_HOST_FLAG_1BIT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = SD_CLK;
    slot.cmd = SD_CMD;
    slot.d0  = SD_DATA0;
    slot.width = 1;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = true,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot,
                                              &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    sdmmc_card_print_info(stdout, s_card);

    /* Create directories */
    mkdir(QUEUE_DIR, 0775);
    mkdir(FAILED_DIR, 0775);

    ESP_LOGI(TAG, "SD card ready, queue at %s", QUEUE_DIR);
    return ESP_OK;
}

esp_err_t sd_queue_add(const char *filepath)
{
    cJSON *root = load_manifest();
    cJSON *pending = cJSON_GetObjectItem(root, "pending");

    cJSON *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "file", filepath);
    cJSON_AddNumberToObject(entry, "ts", (double)time(NULL));
    cJSON_AddNumberToObject(entry, "retries", 0);
    cJSON_AddItemToArray(pending, entry);

    esp_err_t ret = save_manifest(root);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Queued: %s", filepath);
    return ret;
}

static char s_peek_buf[128];

const char *sd_queue_peek(void)
{
    cJSON *root = load_manifest();
    cJSON *pending = cJSON_GetObjectItem(root, "pending");
    int count = cJSON_GetArraySize(pending);
    if (count == 0) {
        cJSON_Delete(root);
        return NULL;
    }

    cJSON *first = cJSON_GetArrayItem(pending, 0);
    cJSON *file = cJSON_GetObjectItem(first, "file");
    if (!file || !file->valuestring) {
        cJSON_Delete(root);
        return NULL;
    }

    strlcpy(s_peek_buf, file->valuestring, sizeof(s_peek_buf));
    cJSON_Delete(root);
    return s_peek_buf;
}

esp_err_t sd_queue_pop(void)
{
    cJSON *root = load_manifest();
    cJSON *pending = cJSON_GetObjectItem(root, "pending");
    if (cJSON_GetArraySize(pending) == 0) {
        cJSON_Delete(root);
        return ESP_OK;
    }

    cJSON *first = cJSON_DetachItemFromArray(pending, 0);
    cJSON *file_j = cJSON_GetObjectItem(first, "file");
    if (file_j && file_j->valuestring) {
        /* Delete the uploaded file */
        remove(file_j->valuestring);
        ESP_LOGI(TAG, "Removed: %s", file_j->valuestring);
    }
    cJSON_Delete(first);

    esp_err_t ret = save_manifest(root);
    cJSON_Delete(root);
    return ret;
}

esp_err_t sd_queue_fail(void)
{
    cJSON *root = load_manifest();
    cJSON *pending = cJSON_GetObjectItem(root, "pending");
    if (cJSON_GetArraySize(pending) == 0) {
        cJSON_Delete(root);
        return ESP_OK;
    }

    cJSON *first = cJSON_GetArrayItem(pending, 0);
    cJSON *retries = cJSON_GetObjectItem(first, "retries");
    int r = retries ? retries->valueint : 0;
    r++;

    if (r >= MAX_RETRIES) {
        /* Move file to /sdcard/failed/ */
        cJSON *file_j = cJSON_GetObjectItem(first, "file");
        if (file_j && file_j->valuestring) {
            const char *src = file_j->valuestring;
            const char *fname = strrchr(src, '/');
            if (fname) fname++; else fname = src;
            char dst[128];
            snprintf(dst, sizeof(dst), "%s/%s", FAILED_DIR, fname);
            rename(src, dst);
            ESP_LOGW(TAG, "Moved to failed: %s", dst);
        }
        cJSON_DetachItemFromArray(pending, 0);
    } else {
        cJSON_ReplaceItemInObject(first, "retries", cJSON_CreateNumber(r));
        ESP_LOGW(TAG, "Retry %d/%d", r, MAX_RETRIES);
    }

    esp_err_t ret = save_manifest(root);
    cJSON_Delete(root);
    return ret;
}

int sd_queue_count(void)
{
    cJSON *root = load_manifest();
    cJSON *pending = cJSON_GetObjectItem(root, "pending");
    int count = cJSON_GetArraySize(pending);
    cJSON_Delete(root);
    return count;
}
