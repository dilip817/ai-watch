#include "wifi_manager.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "esp_http_client.h"
#include "esp_mac.h"
#include "ui_screens.h"

static const char *TAG = "wifi_mgr";

/* Event group bits */
#define WIFI_CONNECTED_BIT   BIT0
#define WIFI_GOT_INTERNET_BIT BIT1
#define WIFI_FAIL_BIT        BIT2

static EventGroupHandle_t s_wifi_event_group;
static wifi_state_t s_state = WIFI_STATE_INIT;
static wifi_state_cb_t s_state_cb = NULL;
static int s_retry_count = 0;
static bool s_has_internet = false;

static const int BACKOFF_TABLE[] = {2, 4, 8, 16, 32, 60};
#define BACKOFF_TABLE_LEN (sizeof(BACKOFF_TABLE) / sizeof(BACKOFF_TABLE[0]))
#define MAX_RETRIES 8

/* NVS keys — multi-credential storage */
#define NVS_NAMESPACE   "wifi_creds"
#define NVS_KEY_COUNT   "count"
#define MAX_SAVED_NETS  5

typedef struct {
    char ssid[33];
    char password[65];
} wifi_cred_t;

static wifi_cred_t s_creds[MAX_SAVED_NETS];
static int s_cred_count = 0;
static int s_cred_index = 0;  /* which credential we're currently trying */

/* Active SSID/password for current connection attempt */
static char s_ssid[33] = {0};
static char s_password[65] = {0};

/* Forward declarations */
static void set_state(wifi_state_t new_state);
static bool check_internet(void);
static void start_softap(void);
static void reconnect_task(void *arg);
static void save_all_credentials(void);
static void try_next_credential(void);

/* ---------- Event handler ---------- */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGW(TAG, "Disconnected from %s", s_ssid);
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_GOT_INTERNET_BIT);
            s_has_internet = false;

            if (s_retry_count < MAX_RETRIES) {
                int idx = s_retry_count < (int)BACKOFF_TABLE_LEN
                          ? s_retry_count : (int)BACKOFF_TABLE_LEN - 1;
                int delay_s = BACKOFF_TABLE[idx];
                ESP_LOGI(TAG, "Retry %d in %ds (network: %s)", s_retry_count + 1, delay_s, s_ssid);
                set_state(WIFI_STATE_RECONNECTING);
                vTaskDelay(pdMS_TO_TICKS(delay_s * 1000));
                esp_wifi_connect();
                s_retry_count++;
            } else {
                /* Try next saved credential before going offline */
                s_cred_index++;
                if (s_cred_index < s_cred_count) {
                    ESP_LOGI(TAG, "Trying next saved network (%d/%d): %s",
                             s_cred_index + 1, s_cred_count, s_creds[s_cred_index].ssid);
                    s_retry_count = 0;
                    try_next_credential();
                } else {
                    ESP_LOGW(TAG, "All %d networks exhausted → OFFLINE", s_cred_count);
                    set_state(WIFI_STATE_OFFLINE);
                    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                    /* Start background reconnect task */
                    xTaskCreate(reconnect_task, "wifi_recon", 4096, NULL, 1, NULL);
                }
            }
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "SoftAP: client connected");
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        set_state(WIFI_STATE_CONNECTED);

        /* Two-level internet check */
        if (check_internet()) {
            s_has_internet = true;
            xEventGroupSetBits(s_wifi_event_group, WIFI_GOT_INTERNET_BIT);
            ESP_LOGI(TAG, "Internet verified");
        } else {
            ESP_LOGW(TAG, "Connected but no internet (captive portal?)");
            set_state(WIFI_STATE_CAPTIVE_PORTAL);
        }

        /* Enable power save */
        esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
    }
}

/* ---------- Internet check ---------- */
static bool check_internet(void)
{
    /* Level 1: TCP connect to 8.8.8.8:53 */
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return false;

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
    };
    inet_pton(AF_INET, "8.8.8.8", &addr.sin_addr);

    struct timeval tv = { .tv_sec = 1, .tv_usec = 500000 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int ret = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    close(sock);
    if (ret != 0) {
        ESP_LOGW(TAG, "TCP 8.8.8.8:53 failed");
        return false;
    }

    /* Level 2: HTTP GET generate_204 (captive portal detection) */
    esp_http_client_config_t cfg = {
        .url = "http://clients3.google.com/generate_204",
        .timeout_ms = 3000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK && status == 204) {
        return true;
    }
    ESP_LOGW(TAG, "generate_204 returned %d", status);
    return false;
}

/* Switch STA config to credential at s_cred_index and connect */
static void try_next_credential(void)
{
    strlcpy(s_ssid, s_creds[s_cred_index].ssid, sizeof(s_ssid));
    strlcpy(s_password, s_creds[s_cred_index].password, sizeof(s_password));

    wifi_config_t sta_cfg = {};
    strlcpy((char *)sta_cfg.sta.ssid, s_ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, s_password, sizeof(sta_cfg.sta.password));

    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    set_state(WIFI_STATE_CONNECTING);
    esp_wifi_connect();
}

/* ---------- OFFLINE reconnect task ---------- */
static void reconnect_task(void *arg)
{
    while (s_state == WIFI_STATE_OFFLINE) {
        vTaskDelay(pdMS_TO_TICKS(5 * 60 * 1000)); /* 5 min sleep */

        /* Scan for any known SSID */
        wifi_scan_config_t scan_cfg = { .show_hidden = true };
        esp_wifi_scan_start(&scan_cfg, true);

        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        if (ap_count == 0) continue;

        wifi_ap_record_t *ap_list = malloc(ap_count * sizeof(wifi_ap_record_t));
        if (!ap_list) continue;
        esp_wifi_scan_get_ap_records(&ap_count, ap_list);

        /* Find best known network (strongest signal among saved) */
        int best_cred = -1;
        int8_t best_rssi = -127;
        for (int i = 0; i < ap_count; i++) {
            for (int c = 0; c < s_cred_count; c++) {
                if (strcmp((char *)ap_list[i].ssid, s_creds[c].ssid) == 0) {
                    if (ap_list[i].rssi > best_rssi) {
                        best_rssi = ap_list[i].rssi;
                        best_cred = c;
                    }
                }
            }
        }
        free(ap_list);

        if (best_cred >= 0) {
            ESP_LOGI(TAG, "Known network visible: %s (rssi=%d), reconnecting",
                     s_creds[best_cred].ssid, best_rssi);
            s_retry_count = 0;
            s_cred_index = best_cred;
            try_next_credential();
            break;
        }
    }
    vTaskDelete(NULL);
}

/* ---------- SoftAP provisioning with WiFi scan ---------- */

#include "cJSON.h"

/* Load all credentials from NVS into s_creds[] */
static void load_all_credentials(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        s_cred_count = 0;
        return;
    }

    /* Try new multi-cred format first */
    uint8_t count = 0;
    if (nvs_get_u8(nvs, NVS_KEY_COUNT, &count) == ESP_OK && count > 0) {
        if (count > MAX_SAVED_NETS) count = MAX_SAVED_NETS;
        s_cred_count = 0;
        for (int i = 0; i < count; i++) {
            char key_s[16], key_p[16];
            snprintf(key_s, sizeof(key_s), "ssid%d", i);
            snprintf(key_p, sizeof(key_p), "pass%d", i);
            size_t slen = sizeof(s_creds[0].ssid);
            size_t plen = sizeof(s_creds[0].password);
            if (nvs_get_str(nvs, key_s, s_creds[s_cred_count].ssid, &slen) == ESP_OK
                && strlen(s_creds[s_cred_count].ssid) > 0) {
                nvs_get_str(nvs, key_p, s_creds[s_cred_count].password, &plen);
                s_cred_count++;
            }
        }
        nvs_close(nvs);
        ESP_LOGI(TAG, "Loaded %d saved networks", s_cred_count);
        return;
    }

    /* Migrate legacy single-cred format */
    char legacy_ssid[33] = {0};
    char legacy_pass[65] = {0};
    size_t slen = sizeof(legacy_ssid), plen = sizeof(legacy_pass);
    if (nvs_get_str(nvs, "ssid", legacy_ssid, &slen) == ESP_OK
        && strlen(legacy_ssid) > 0) {
        nvs_get_str(nvs, "password", legacy_pass, &plen);
        strlcpy(s_creds[0].ssid, legacy_ssid, sizeof(s_creds[0].ssid));
        strlcpy(s_creds[0].password, legacy_pass, sizeof(s_creds[0].password));
        s_cred_count = 1;
        nvs_close(nvs);
        ESP_LOGI(TAG, "Migrated legacy credential: %s", legacy_ssid);
        /* Save in new format and clean up legacy keys */
        save_all_credentials();
        return;
    }

    nvs_close(nvs);
    s_cred_count = 0;
}

/* Save all credentials from s_creds[] to NVS */
static void save_all_credentials(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;

    nvs_set_u8(nvs, NVS_KEY_COUNT, (uint8_t)s_cred_count);
    for (int i = 0; i < s_cred_count; i++) {
        char key_s[16], key_p[16];
        snprintf(key_s, sizeof(key_s), "ssid%d", i);
        snprintf(key_p, sizeof(key_p), "pass%d", i);
        nvs_set_str(nvs, key_s, s_creds[i].ssid);
        nvs_set_str(nvs, key_p, s_creds[i].password);
    }
    /* Erase unused slots */
    for (int i = s_cred_count; i < MAX_SAVED_NETS; i++) {
        char key_s[16], key_p[16];
        snprintf(key_s, sizeof(key_s), "ssid%d", i);
        snprintf(key_p, sizeof(key_p), "pass%d", i);
        nvs_erase_key(nvs, key_s);
        nvs_erase_key(nvs, key_p);
    }
    /* Clean up legacy keys */
    nvs_erase_key(nvs, "ssid");
    nvs_erase_key(nvs, "password");

    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "Saved %d networks to NVS", s_cred_count);
}

/* Add or promote a network to slot 0 (most recent) */
static void save_credentials(const char *ssid, const char *pass)
{
    /* Check if already saved — remove old entry */
    int existing = -1;
    for (int i = 0; i < s_cred_count; i++) {
        if (strcmp(s_creds[i].ssid, ssid) == 0) {
            existing = i;
            break;
        }
    }

    if (existing > 0) {
        /* Shift entries down to remove old position */
        wifi_cred_t tmp = s_creds[existing];
        for (int i = existing; i > 0; i--) {
            s_creds[i] = s_creds[i - 1];
        }
        s_creds[0] = tmp;
        /* Update password in case it changed */
        strlcpy(s_creds[0].password, pass, sizeof(s_creds[0].password));
    } else if (existing == 0) {
        /* Already at slot 0, just update password */
        strlcpy(s_creds[0].password, pass, sizeof(s_creds[0].password));
    } else {
        /* New network — shift everything down, insert at 0 */
        int new_count = s_cred_count < MAX_SAVED_NETS ? s_cred_count + 1 : MAX_SAVED_NETS;
        for (int i = new_count - 1; i > 0; i--) {
            s_creds[i] = s_creds[i - 1];
        }
        strlcpy(s_creds[0].ssid, ssid, sizeof(s_creds[0].ssid));
        strlcpy(s_creds[0].password, pass, sizeof(s_creds[0].password));
        s_cred_count = new_count;
    }

    save_all_credentials();
    ESP_LOGI(TAG, "Credentials saved for: %s (%d total)", ssid, s_cred_count);
}

/* ── /scan endpoint — triggers WiFi scan, returns JSON ── */

static esp_err_t scan_handler(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_wifi_scan_start(&scan_cfg, true);

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);

    uint16_t fetch_count = ap_count < 20 ? ap_count : 20;
    wifi_ap_record_t *ap_list = calloc(fetch_count, sizeof(wifi_ap_record_t));
    if (!ap_list) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    esp_wifi_scan_get_ap_records(&fetch_count, ap_list);

    cJSON *root = cJSON_CreateArray();
    for (int i = 0; i < fetch_count; i++) {
        if (strlen((char *)ap_list[i].ssid) == 0) continue;

        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "ssid", (char *)ap_list[i].ssid);
        cJSON_AddNumberToObject(entry, "rssi", ap_list[i].rssi);
        cJSON_AddBoolToObject(entry, "secure",
            ap_list[i].authmode != WIFI_AUTH_OPEN);

        int8_t rssi = ap_list[i].rssi;
        int bars = rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1;
        cJSON_AddNumberToObject(entry, "bars", bars);

        cJSON_AddItemToArray(root, entry);
    }
    free(ap_list);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, json_str);
    free(json_str);

    ESP_LOGI(TAG, "Scan returned %d networks", (int)fetch_count);
    return ESP_OK;
}

/* ── /connect endpoint — receives JSON {ssid, password}, saves, reboots ── */

static esp_err_t connect_handler(httpd_req_t *req)
{
    char buf[256] = {0};
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *body = cJSON_Parse(buf);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *ssid_j = cJSON_GetObjectItem(body, "ssid");
    cJSON *pass_j = cJSON_GetObjectItem(body, "password");

    if (!ssid_j || !cJSON_IsString(ssid_j) || strlen(ssid_j->valuestring) == 0) {
        cJSON_Delete(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
        return ESP_FAIL;
    }

    const char *ssid = ssid_j->valuestring;
    const char *pass = (pass_j && cJSON_IsString(pass_j)) ? pass_j->valuestring : "";

    save_credentials(ssid, pass);
    cJSON_Delete(body);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"Connecting...\"}");

    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

/* ── /saved endpoint — returns saved networks (without passwords) ── */

static esp_err_t saved_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateArray();
    for (int i = 0; i < s_cred_count; i++) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "ssid", s_creds[i].ssid);
        cJSON_AddNumberToObject(entry, "index", i);
        cJSON_AddItemToArray(root, entry);
    }
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    return ESP_OK;
}

/* ── /forget endpoint — removes a saved network by SSID ── */

static esp_err_t forget_handler(httpd_req_t *req)
{
    char buf[128] = {0};
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *body = cJSON_Parse(buf);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *ssid_j = cJSON_GetObjectItem(body, "ssid");
    if (!ssid_j || !cJSON_IsString(ssid_j)) {
        cJSON_Delete(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
        return ESP_FAIL;
    }

    const char *ssid = ssid_j->valuestring;
    bool found = false;
    for (int i = 0; i < s_cred_count; i++) {
        if (strcmp(s_creds[i].ssid, ssid) == 0) {
            /* Shift remaining entries up */
            for (int j = i; j < s_cred_count - 1; j++) {
                s_creds[j] = s_creds[j + 1];
            }
            s_cred_count--;
            memset(&s_creds[s_cred_count], 0, sizeof(wifi_cred_t));
            found = true;
            break;
        }
    }
    cJSON_Delete(body);

    if (found) {
        save_all_credentials();
        ESP_LOGI(TAG, "Forgot network: %s (%d remaining)", ssid, s_cred_count);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Network not found");
    }
    return ESP_OK;
}

/* ── Provisioning HTML page — modern SPA with WiFi scanning ── */

static const char PROV_PAGE[] =
"<!DOCTYPE html>"
"<html><head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Student Assistant Setup</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;"
     "background:#0a0a0a;color:#e8e8e8;padding:24px 16px;min-height:100vh}"
"h1{font-size:20px;font-weight:700;color:#1D9E75;margin-bottom:4px}"
".sub{font-size:13px;color:#666;margin-bottom:24px}"
".section{font-size:11px;font-weight:700;color:#555;letter-spacing:.08em;"
         "text-transform:uppercase;margin-bottom:8px}"
".network-list{display:flex;flex-direction:column;gap:8px;margin-bottom:24px}"
".network-item{"
  "display:flex;align-items:center;justify-content:space-between;"
  "background:#111827;border:1px solid #1e2a3a;border-radius:10px;"
  "padding:12px 14px;cursor:pointer;transition:all .15s}"
".network-item:active{background:#1a2538}"
".network-item.selected{border-color:#1D9E75;background:#0a1f14}"
".net-info{display:flex;flex-direction:column;gap:2px}"
".net-name{font-size:14px;font-weight:600;color:#e8e8e8}"
".net-meta{font-size:11px;color:#555}"
".bars{display:flex;align-items:flex-end;gap:2px;height:16px}"
".bar{width:4px;border-radius:1px;background:#333}"
".bar.active{background:#1D9E75}"
".b1{height:4px}.b2{height:8px}.b3{height:12px}.b4{height:16px}"
"label{display:block;font-size:13px;color:#888;margin-bottom:6px}"
"input{width:100%;padding:12px 14px;background:#111827;"
      "border:1px solid #1e2a3a;border-radius:10px;color:#e8e8e8;"
      "font-size:15px;margin-bottom:16px;outline:none}"
"input:focus{border-color:#1D9E75}"
"button{width:100%;padding:14px;background:#1D9E75;border:none;"
       "border-radius:10px;color:#fff;font-size:15px;font-weight:700;"
       "cursor:pointer;transition:opacity .15s}"
"button:active{opacity:.8}"
"button:disabled{opacity:.4;cursor:default}"
".status{text-align:center;font-size:13px;color:#888;margin-top:16px;min-height:20px}"
".status.ok{color:#1D9E75}"
".status.err{color:#E24B4A}"
".lock{font-size:10px;margin-left:4px;opacity:.6}"
".scanning{font-size:13px;color:#555;text-align:center;padding:20px 0}"
"</style></head><body>"
"<h1>Student Assistant</h1>"
"<p class='sub'>Connect to your home WiFi to get started</p>"
"<div class='section'>Available networks</div>"
"<div class='network-list' id='networks'>"
  "<div class='scanning'>Scanning...</div>"
"</div>"
"<div class='section'>WiFi password</div>"
"<label>Network</label>"
"<input type='text' id='ssid' placeholder='Tap a network above'>"
"<label>Password</label>"
"<input type='password' id='password' placeholder='Enter password'>"
"<button id='connect-btn' onclick='doConnect()' disabled>Connect</button>"
"<div class='status' id='status'></div>"
"<div id='saved-section' style='display:none;margin-top:24px'>"
  "<div class='section'>Saved networks</div>"
  "<div class='network-list' id='saved-nets'></div>"
"</div>"
"<script>"
"function bars(n){"
  "let h='';"
  "for(let i=1;i<=4;i++)"
    "h+='<div class=\"bar b'+i+(i<=n?' active':'')+'\"></div>';"
  "return '<div class=\"bars\">'+h+'</div>';"
"}"
"async function scan(){"
  "try{"
    "const r=await fetch('/scan');"
    "const nets=await r.json();"
    "const el=document.getElementById('networks');"
    "if(!nets.length){"
      "el.innerHTML='<div class=\"scanning\">No networks found &mdash; '"
        "+'<span onclick=\"scan()\" style=\"color:#7F77DD;cursor:pointer\">"
        "tap to retry</span></div>';"
      "return;"
    "}"
    "el.innerHTML=nets.map(function(n){"
      "return '<div class=\"network-item\" onclick=\"sel(\\''+n.ssid+'\\')\">'"
        "+'<div class=\"net-info\"><div class=\"net-name\">'+n.ssid"
        "+(n.secure?'<span class=\"lock\">&#128274;</span>':'')"
        "+'</div><div class=\"net-meta\">'+(n.rssi>-55?'Excellent':n.rssi>-65?'Good':n.rssi>-75?'Fair':'Weak')"
        "+' &middot; '+n.rssi+'dBm</div></div>'+bars(n.bars)+'</div>';"
    "}).join('');"
  "}catch(e){"
    "document.getElementById('networks').innerHTML="
      "'<div class=\"scanning\">Scan failed &mdash; check connection</div>';"
  "}"
"}"
"function sel(ssid){"
  "document.getElementById('ssid').value=ssid;"
  "document.getElementById('connect-btn').disabled=false;"
  "document.querySelectorAll('.network-item').forEach(function(el){"
    "el.classList.toggle('selected',"
      "el.querySelector('.net-name').textContent.indexOf(ssid)===0);"
  "});"
  "document.getElementById('password').focus();"
"}"
"async function doConnect(){"
  "var ssid=document.getElementById('ssid').value.trim();"
  "var pass=document.getElementById('password').value;"
  "if(!ssid)return;"
  "var btn=document.getElementById('connect-btn');"
  "var st=document.getElementById('status');"
  "btn.disabled=true;btn.textContent='Connecting...';st.textContent='';"
  "try{"
    "var r=await fetch('/connect',{"
      "method:'POST',"
      "headers:{'Content-Type':'application/json'},"
      "body:JSON.stringify({ssid:ssid,password:pass})"
    "});"
    "var d=await r.json();"
    "if(d.ok){"
      "st.className='status ok';"
      "st.textContent='Saved! Device is restarting...';"
      "btn.textContent='Done';"
    "}else{throw new Error(d.message||'Failed');}"
  "}catch(e){"
    "st.className='status err';"
    "st.textContent='Error: '+e.message;"
    "btn.disabled=false;btn.textContent='Try again';"
  "}"
"}"
"document.getElementById('ssid').addEventListener('input',function(){"
  "document.getElementById('connect-btn').disabled=!this.value.trim();"
"});"
"async function loadSaved(){"
  "try{"
    "const r=await fetch('/saved');"
    "const nets=await r.json();"
    "if(!nets.length)return;"
    "document.getElementById('saved-section').style.display='block';"
    "document.getElementById('saved-nets').innerHTML=nets.map(function(n){"
      "return '<div class=\"network-item\" style=\"justify-content:space-between\">"
        "<div class=\"net-info\"><div class=\"net-name\">'+n.ssid+'</div>"
        "<div class=\"net-meta\">Saved</div></div>"
        "<span onclick=\"forget(\\''+n.ssid+'\\')\" "
        "style=\"color:#E24B4A;font-size:12px;font-weight:600;cursor:pointer\">"
        "Forget</span></div>';"
    "}).join('');"
  "}catch(e){}"
"}"
"async function forget(ssid){"
  "if(!confirm('Forget '+ssid+'?'))return;"
  "try{"
    "await fetch('/forget',{"
      "method:'POST',"
      "headers:{'Content-Type':'application/json'},"
      "body:JSON.stringify({ssid:ssid})"
    "});"
    "loadSaved();"
  "}catch(e){}"
"}"
"scan();loadSaved();"
"</script></body></html>";

static esp_err_t page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    /* Send in chunks to handle large page */
    httpd_resp_send(req, PROV_PAGE, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void start_softap(void)
{
    set_state(WIFI_STATE_PROVISIONING);

    /* Generate unique AP name from MAC */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "StudentDevice-%02X%02X", mac[4], mac[5]);

    wifi_config_t ap_cfg = {
        .ap = {
            .max_connection = 2,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    strlcpy((char *)ap_cfg.ap.ssid, ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(ap_ssid);

    /* Show provisioning UI on watch display */
    ui_show_provisioning_screen(ap_ssid);

    /* Use APSTA mode so WiFi scan works while AP is active */
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    esp_wifi_start();
    ESP_LOGI(TAG, "SoftAP started: %s (APSTA mode for scanning)", ap_ssid);

    /* Start HTTP server with scan + connect endpoints */
    httpd_config_t httpd_cfg = HTTPD_DEFAULT_CONFIG();
    httpd_cfg.max_uri_handlers = 8;
    httpd_handle_t server = NULL;
    httpd_start(&server, &httpd_cfg);

    httpd_uri_t uri_page = { .uri = "/", .method = HTTP_GET, .handler = page_handler };
    httpd_uri_t uri_scan = { .uri = "/scan", .method = HTTP_GET, .handler = scan_handler };
    httpd_uri_t uri_connect = { .uri = "/connect", .method = HTTP_POST, .handler = connect_handler };
    httpd_uri_t uri_saved = { .uri = "/saved", .method = HTTP_GET, .handler = saved_handler };
    httpd_uri_t uri_forget = { .uri = "/forget", .method = HTTP_POST, .handler = forget_handler };
    httpd_register_uri_handler(server, &uri_page);
    httpd_register_uri_handler(server, &uri_scan);
    httpd_register_uri_handler(server, &uri_connect);
    httpd_register_uri_handler(server, &uri_saved);
    httpd_register_uri_handler(server, &uri_forget);
}

/* ---------- State management ---------- */
static void set_state(wifi_state_t new_state)
{
    if (s_state == new_state) return;
    ESP_LOGI(TAG, "State: %d → %d", s_state, new_state);
    s_state = new_state;
    if (s_state_cb) s_state_cb(new_state);
}

/* ---------- Public API ---------- */

esp_err_t wifi_manager_init(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t inst_any, inst_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, &inst_any);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, NULL, &inst_ip);

    /* Load all saved credentials from NVS */
    load_all_credentials();

    if (s_cred_count > 0) {
        for (int i = 0; i < s_cred_count; i++) {
            ESP_LOGI(TAG, "  Saved network %d: %s", i, s_creds[i].ssid);
        }

        /* Start with first (most recent) credential */
        s_cred_index = 0;
        strlcpy(s_ssid, s_creds[0].ssid, sizeof(s_ssid));
        strlcpy(s_password, s_creds[0].password, sizeof(s_password));

        wifi_config_t sta_cfg = {};
        strlcpy((char *)sta_cfg.sta.ssid, s_ssid, sizeof(sta_cfg.sta.ssid));
        strlcpy((char *)sta_cfg.sta.password, s_password, sizeof(sta_cfg.sta.password));

        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        esp_wifi_start();
        set_state(WIFI_STATE_CONNECTING);

        /* Wait for connection with 10s timeout */
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(10000));

        if (bits & WIFI_CONNECTED_BIT) {
            return ESP_OK;
        }
        /* Fall through — will retry via event handler, cycling through credentials */
        return ESP_OK;
    }

    /* No credentials — start provisioning */
    ESP_LOGI(TAG, "No WiFi credentials, starting SoftAP");
    start_softap();
    return ESP_OK;
}

void wifi_manager_register_cb(wifi_state_cb_t cb)
{
    s_state_cb = cb;
}

wifi_state_t wifi_manager_get_state(void)
{
    return s_state;
}

bool wifi_manager_has_internet(void)
{
    return s_has_internet;
}
