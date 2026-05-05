#include "web_server.h"
#include "ota_manager.h"
#include "wifi_manager.h"
#include "boot_manager.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "cJSON.h"

static const char *TAG = "WEB_SRV";

#define SPIFFS_BASE_PATH    "/spiffs"
#define SCRATCH_BUFSIZE     (8192)
#define OTA_UPLOAD_BUF_SIZE (4096)

static httpd_handle_t s_server = NULL;
static char s_scratch[SCRATCH_BUFSIZE];
static bool s_spiffs_mounted = false;

/* ── SPIFFS ───────────────────────────────────────────────────────────────── */

esp_err_t web_server_mount_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = SPIFFS_BASE_PATH,
        .partition_label        = "spiffs",
        .max_files              = 10,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info("spiffs", &total, &used);
    ESP_LOGI(TAG, "SPIFFS mounted: %zu total, %zu used bytes", total, used);
    s_spiffs_mounted = true;
    return ESP_OK;
}

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static esp_err_t send_json_response(httpd_req_t *req, cJSON *root, int status_code)
{
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON error");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    if (status_code != 200) {
        char status_str[8];
        snprintf(status_str, sizeof(status_str), "%d", status_code);
        httpd_resp_set_status(req, status_code == 400 ? "400 Bad Request" :
                                   status_code == 500 ? "500 Internal Server Error" :
                                   "200 OK");
    }

    esp_err_t ret = httpd_resp_sendstr(req, json_str);
    free(json_str);
    return ret;
}

static void send_file(httpd_req_t *req, const char *path, const char *mime)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGW(TAG, "File not found: %s", path);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return;
    }

    httpd_resp_set_type(req, mime);
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");

    size_t rd;
    do {
        rd = fread(s_scratch, 1, sizeof(s_scratch), f);
        if (rd > 0) {
            httpd_resp_send_chunk(req, s_scratch, rd);
        }
    } while (rd == sizeof(s_scratch));

    httpd_resp_send_chunk(req, NULL, 0);
    fclose(f);
}

/* ── API: GET /api/status ─────────────────────────────────────────────────── */

static esp_err_t handle_api_status(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    /* App version */
    esp_app_desc_t app_desc = {0};
    ota_manager_get_app_desc(&app_desc);
    cJSON_AddStringToObject(root, "version",        app_desc.version[0] ? app_desc.version : "unknown");
    cJSON_AddStringToObject(root, "project_name",   app_desc.project_name[0] ? app_desc.project_name : "unknown");
    cJSON_AddStringToObject(root, "idf_version",    app_desc.idf_ver[0] ? app_desc.idf_ver : esp_get_idf_version());
    cJSON_AddStringToObject(root, "compile_date",   app_desc.date[0] ? app_desc.date : "unknown");
    cJSON_AddStringToObject(root, "compile_time",   app_desc.time[0] ? app_desc.time : "unknown");

    /* Partitions */
    cJSON_AddStringToObject(root, "running_partition", ota_manager_get_running_partition_label());
    cJSON_AddStringToObject(root, "next_partition",    ota_manager_get_next_partition_label());
    cJSON_AddBoolToObject(root,   "next_partition_valid", ota_manager_next_partition_valid());

    /* WiFi */
    wifi_sta_state_t sta = wifi_manager_get_sta_state();
    const char *sta_str = (sta == WIFI_STA_CONNECTED)   ? "connected" :
                          (sta == WIFI_STA_CONNECTING)   ? "connecting" :
                          (sta == WIFI_STA_FAILED)       ? "failed" : "disconnected";
    cJSON_AddStringToObject(root, "sta_state", sta_str);

    const char *sta_ip = wifi_manager_get_sta_ip();
    cJSON_AddStringToObject(root, "sta_ip",  sta_ip ? sta_ip : "");
    cJSON_AddStringToObject(root, "ap_ip",   wifi_manager_get_ap_ip());
    cJSON_AddNumberToObject(root, "ap_clients", wifi_manager_get_ap_client_count());

    /* System */
    cJSON_AddNumberToObject(root, "free_heap",   esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_heap",    esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "uptime_ms",   (double)(esp_timer_get_time() / 1000));
    cJSON_AddNumberToObject(root, "fail_count",  boot_manager_get_fail_count());

    return send_json_response(req, root, 200);
}

/* ── API: POST /api/ota/url ───────────────────────────────────────────────── */

static esp_err_t handle_ota_from_url(httpd_req_t *req)
{
    int content_len = req->content_len;
    if (content_len <= 0 || content_len >= (int)sizeof(s_scratch)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    int received = httpd_req_recv(req, s_scratch, content_len);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    s_scratch[received] = '\0';

    cJSON *body = cJSON_Parse(s_scratch);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *url_item = cJSON_GetObjectItem(body, "url");
    if (!cJSON_IsString(url_item) || strlen(url_item->valuestring) == 0) {
        cJSON_Delete(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'url' field");
        return ESP_FAIL;
    }

    char url[512];
    strlcpy(url, url_item->valuestring, sizeof(url));
    cJSON_Delete(body);

    ESP_LOGI(TAG, "OTA from URL requested: %s", url);

    /* Send immediate ACK, then do OTA */
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "started");
    cJSON_AddStringToObject(resp, "url", url);
    send_json_response(req, resp, 200);

    /* Perform OTA (blocks until done) */
    esp_err_t ret = ota_manager_update_from_url(url, NULL);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA URL complete, rebooting in 2s");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA URL failed: %s", esp_err_to_name(ret));
    }

    return ESP_OK;
}

/* ── API: POST /api/ota/upload (multipart binary stream) ─────────────────── */

static esp_err_t handle_ota_upload(httpd_req_t *req)
{
    int content_len = req->content_len;
    if (content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content length");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA upload: expected %d bytes", content_len);

    esp_err_t ret = ota_manager_begin_upload((size_t)content_len);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "OTA begin failed");
        return ESP_FAIL;
    }

    static uint8_t ota_buf[OTA_UPLOAD_BUF_SIZE];
    int remaining = content_len;
    int written_total = 0;

    while (remaining > 0) {
        int to_read = (remaining < (int)sizeof(ota_buf)) ? remaining : (int)sizeof(ota_buf);
        int rd = httpd_req_recv(req, (char *)ota_buf, to_read);
        if (rd <= 0) {
            if (rd == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "OTA recv error: %d", rd);
            ota_manager_abort_upload();
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }

        ret = ota_manager_write_chunk(ota_buf, rd);
        if (ret != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
            return ESP_FAIL;
        }

        written_total += rd;
        remaining -= rd;
    }

    ret = ota_manager_end_upload();
    if (ret != ESP_OK) {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "status", "error");
        cJSON_AddStringToObject(resp, "message", esp_err_to_name(ret));
        send_json_response(req, resp, 500);
        return ESP_FAIL;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "success");
    cJSON_AddNumberToObject(resp, "bytes_written", written_total);
    cJSON_AddStringToObject(resp, "message", "Firmware flashed. Rebooting...");
    send_json_response(req, resp, 200);

    ESP_LOGI(TAG, "OTA upload complete (%d bytes), rebooting", written_total);
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();

    return ESP_OK;
}

/* ── API: POST /api/partition/switch ─────────────────────────────────────── */

static esp_err_t handle_partition_switch(httpd_req_t *req)
{
    if (!ota_manager_next_partition_valid()) {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "status", "error");
        cJSON_AddStringToObject(resp, "message", "Next partition has no valid firmware");
        return send_json_response(req, resp, 400);
    }

    esp_err_t ret = ota_manager_switch_partition();
    cJSON *resp = cJSON_CreateObject();

    if (ret == ESP_OK) {
        cJSON_AddStringToObject(resp, "status", "success");
        cJSON_AddStringToObject(resp, "message", "Partition switched. Rebooting...");
        cJSON_AddStringToObject(resp, "target_partition", ota_manager_get_next_partition_label());
        send_json_response(req, resp, 200);
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    } else {
        cJSON_AddStringToObject(resp, "status", "error");
        cJSON_AddStringToObject(resp, "message", esp_err_to_name(ret));
        send_json_response(req, resp, 500);
    }

    return ESP_OK;
}

/* ── API: POST /api/wifi/config ───────────────────────────────────────────── */

static esp_err_t handle_wifi_config(httpd_req_t *req)
{
    int content_len = req->content_len;
    if (content_len <= 0 || content_len >= (int)sizeof(s_scratch)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    int received = httpd_req_recv(req, s_scratch, content_len);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    s_scratch[received] = '\0';

    cJSON *body = cJSON_Parse(s_scratch);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *ssid_item = cJSON_GetObjectItem(body, "ssid");
    cJSON *pass_item = cJSON_GetObjectItem(body, "password");

    if (!cJSON_IsString(ssid_item) || strlen(ssid_item->valuestring) == 0) {
        cJSON_Delete(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'ssid'");
        return ESP_FAIL;
    }

    char ssid[WIFI_SSID_MAX_LEN]     = {0};
    char pass[WIFI_PASSWORD_MAX_LEN] = {0};
    strlcpy(ssid, ssid_item->valuestring, sizeof(ssid));
    if (cJSON_IsString(pass_item)) {
        strlcpy(pass, pass_item->valuestring, sizeof(pass));
    }
    cJSON_Delete(body);

    /* Save to NVS */
    esp_err_t ret = wifi_manager_save_credentials(ssid, pass);
    if (ret != ESP_OK) {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "status", "error");
        cJSON_AddStringToObject(resp, "message", "Failed to save credentials");
        return send_json_response(req, resp, 500);
    }

    /* Reconnect STA */
    wifi_manager_reconnect_sta(ssid, pass);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "success");
    cJSON_AddStringToObject(resp, "ssid", ssid);
    cJSON_AddStringToObject(resp, "message", "Credentials saved. Reconnecting...");
    return send_json_response(req, resp, 200);
}

/* ── API: GET /api/wifi/status ────────────────────────────────────────────── */

static esp_err_t handle_wifi_status(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    wifi_sta_state_t sta = wifi_manager_get_sta_state();
    const char *sta_str = (sta == WIFI_STA_CONNECTED)   ? "connected" :
                          (sta == WIFI_STA_CONNECTING)   ? "connecting" :
                          (sta == WIFI_STA_FAILED)       ? "failed" : "disconnected";
    cJSON_AddStringToObject(root, "sta_state", sta_str);

    const char *sta_ip = wifi_manager_get_sta_ip();
    cJSON_AddStringToObject(root, "sta_ip", sta_ip ? sta_ip : "");
    cJSON_AddStringToObject(root, "ap_ip", wifi_manager_get_ap_ip());
    cJSON_AddNumberToObject(root, "ap_clients", wifi_manager_get_ap_client_count());

    /* Return saved SSID */
    char saved_ssid[WIFI_SSID_MAX_LEN] = {0};
    char dummy_pass[WIFI_PASSWORD_MAX_LEN] = {0};
    size_t sl = sizeof(saved_ssid), pl = sizeof(dummy_pass);
    if (wifi_manager_load_credentials(saved_ssid, sl, dummy_pass, pl) == ESP_OK) {
        cJSON_AddStringToObject(root, "saved_ssid", saved_ssid);
    } else {
        cJSON_AddStringToObject(root, "saved_ssid", "");
    }

    return send_json_response(req, root, 200);
}

/* ── API: POST /api/reboot ────────────────────────────────────────────────── */

static esp_err_t handle_reboot(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "rebooting");
    send_json_response(req, resp, 200);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

/* ── Static file handlers ─────────────────────────────────────────────────── */

static esp_err_t handle_index(httpd_req_t *req)
{
    if (s_spiffs_mounted) {
        send_file(req, SPIFFS_BASE_PATH "/index.html", "text/html");
    } else {
        /* Fallback inline minimal HTML if SPIFFS not available */
        httpd_resp_set_type(req, "text/html");
        httpd_resp_sendstr(req,
            "<!DOCTYPE html><html><head><meta charset='utf-8'>"
            "<title>ESP32 Boot Manager</title>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<style>body{font-family:monospace;background:#0a0a0a;color:#00ff88;padding:20px}"
            "h1{color:#00ffcc}a{color:#00aaff;display:block;margin:8px 0;font-size:1.2em}"
            "</style></head><body>"
            "<h1>&#9889; ESP32 Boot Manager</h1>"
            "<p>SPIFFS not mounted. Web UI unavailable.</p>"
            "<p>API endpoints:</p>"
            "<a href='/api/status'>/api/status</a>"
            "</body></html>");
    }
    return ESP_OK;
}

static esp_err_t handle_css(httpd_req_t *req)
{
    send_file(req, SPIFFS_BASE_PATH "/style.css", "text/css");
    return ESP_OK;
}

static esp_err_t handle_js(httpd_req_t *req)
{
    send_file(req, SPIFFS_BASE_PATH "/app.js", "application/javascript");
    return ESP_OK;
}

/* ── CORS preflight ───────────────────────────────────────────────────────── */

static esp_err_t handle_options(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ── URI table ────────────────────────────────────────────────────────────── */

static const httpd_uri_t uri_table[] = {
    { .uri = "/",                    .method = HTTP_GET,  .handler = handle_index         },
    { .uri = "/style.css",           .method = HTTP_GET,  .handler = handle_css           },
    { .uri = "/app.js",              .method = HTTP_GET,  .handler = handle_js            },
    { .uri = "/api/status",          .method = HTTP_GET,  .handler = handle_api_status    },
    { .uri = "/api/wifi/status",     .method = HTTP_GET,  .handler = handle_wifi_status   },
    { .uri = "/api/ota/url",         .method = HTTP_POST, .handler = handle_ota_from_url  },
    { .uri = "/api/ota/upload",      .method = HTTP_POST, .handler = handle_ota_upload    },
    { .uri = "/api/partition/switch",.method = HTTP_POST, .handler = handle_partition_switch },
    { .uri = "/api/wifi/config",     .method = HTTP_POST, .handler = handle_wifi_config   },
    { .uri = "/api/reboot",          .method = HTTP_POST, .handler = handle_reboot        },
    { .uri = "/api/status",          .method = HTTP_OPTIONS, .handler = handle_options    },
    { .uri = "/api/ota/url",         .method = HTTP_OPTIONS, .handler = handle_options    },
    { .uri = "/api/ota/upload",      .method = HTTP_OPTIONS, .handler = handle_options    },
    { .uri = "/api/partition/switch",.method = HTTP_OPTIONS, .handler = handle_options    },
    { .uri = "/api/wifi/config",     .method = HTTP_OPTIONS, .handler = handle_options    },
    { .uri = "/api/reboot",          .method = HTTP_OPTIONS, .handler = handle_options    },
};

/* ── Start / Stop ─────────────────────────────────────────────────────────── */

esp_err_t web_server_start(void)
{
    if (s_server) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable   = true;
    config.max_uri_handlers   = 20;
    config.stack_size         = 8192;
    config.max_open_sockets   = 5;
    config.recv_wait_timeout  = 10;
    config.send_wait_timeout  = 10;

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Register all URI handlers */
    for (int i = 0; i < (int)(sizeof(uri_table) / sizeof(uri_table[0])); i++) {
        ret = httpd_register_uri_handler(s_server, &uri_table[i]);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to register URI %s: %s",
                     uri_table[i].uri, esp_err_to_name(ret));
        }
    }

    ESP_LOGI(TAG, "HTTP server started on port 80");
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (!s_server) return ESP_OK;
    esp_err_t ret = httpd_stop(s_server);
    s_server = NULL;
    return ret;
}
