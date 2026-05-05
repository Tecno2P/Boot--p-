#include "wifi_manager.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "WIFI_MGR";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t   s_wifi_event_group = NULL;
static esp_netif_t         *s_ap_netif  = NULL;
static esp_netif_t         *s_sta_netif = NULL;
static wifi_sta_state_t     s_sta_state = WIFI_STA_DISCONNECTED;
static int                  s_retry_num = 0;
static char                 s_sta_ip[16]  = {0};
static char                 s_ap_ip[16]   = "192.168.4.1";
static bool                 s_initialized = false;

/* ── Event Handlers ───────────────────────────────────────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA started, connecting...");
            s_sta_state = WIFI_STA_CONNECTING;
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *disc =
                (wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGW(TAG, "STA disconnected (reason=%d), retry %d/%d",
                     disc->reason, s_retry_num, WIFI_STA_MAX_RETRY);
            s_sta_state = WIFI_STA_CONNECTING;
            memset(s_sta_ip, 0, sizeof(s_sta_ip));

            if (s_retry_num < WIFI_STA_MAX_RETRY) {
                s_retry_num++;
                vTaskDelay(pdMS_TO_TICKS(2000 * s_retry_num)); /* back-off */
                esp_wifi_connect();
            } else {
                ESP_LOGW(TAG, "STA max retries reached — AP remains active");
                s_sta_state = WIFI_STA_FAILED;
                if (s_wifi_event_group) {
                    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                }
            }
            break;
        }

        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *ev =
                (wifi_event_ap_staconnected_t *)event_data;
            ESP_LOGI(TAG, "AP: client connected, MAC=" MACSTR " AID=%d",
                     MAC2STR(ev->mac), ev->aid);
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *ev =
                (wifi_event_ap_stadisconnected_t *)event_data;
            ESP_LOGI(TAG, "AP: client disconnected, MAC=" MACSTR " AID=%d",
                     MAC2STR(ev->mac), ev->aid);
            break;
        }

        default:
            break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
            snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&ev->ip_info.ip));
            ESP_LOGI(TAG, "STA got IP: %s", s_sta_ip);
            s_sta_state = WIFI_STA_CONNECTED;
            s_retry_num = 0;
            if (s_wifi_event_group) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            }
        }
    }
}

/* ── Init ─────────────────────────────────────────────────────────────────── */

esp_err_t wifi_manager_init(const wifi_manager_config_t *cfg)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    if (!cfg) return ESP_ERR_INVALID_ARG;

    ESP_ERROR_CHECK(esp_netif_init());

    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
        return ESP_ERR_NO_MEM;
    }

    /* Create default AP and STA netif */
    s_ap_netif  = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    if (!s_ap_netif || !s_sta_netif) {
        ESP_LOGE(TAG, "Failed to create netif instances");
        return ESP_FAIL;
    }

    /* Init WiFi with default config */
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    /* Register event handlers */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                         ESP_EVENT_ANY_ID,
                                                         &wifi_event_handler,
                                                         NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                         IP_EVENT_STA_GOT_IP,
                                                         &wifi_event_handler,
                                                         NULL, NULL));

    /* Set AP+STA mode */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    /* Configure AP */
    wifi_config_t ap_conf = {
        .ap = {
            .channel        = cfg->ap_channel,
            .max_connection = cfg->ap_max_conn,
            .authmode       = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg        = {
                .required = false,
            },
        },
    };
    strlcpy((char *)ap_conf.ap.ssid,     cfg->ap_ssid,     sizeof(ap_conf.ap.ssid));
    strlcpy((char *)ap_conf.ap.password, cfg->ap_password,  sizeof(ap_conf.ap.password));
    ap_conf.ap.ssid_len = (uint8_t)strlen(cfg->ap_ssid);

    if (strlen(cfg->ap_password) == 0) {
        ap_conf.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_conf));

    /* Load STA credentials from NVS */
    char sta_ssid[WIFI_SSID_MAX_LEN]     = {0};
    char sta_pass[WIFI_PASSWORD_MAX_LEN] = {0};
    esp_err_t cred_ret = wifi_manager_load_credentials(sta_ssid, sizeof(sta_ssid),
                                                        sta_pass, sizeof(sta_pass));
    if (cred_ret == ESP_OK && strlen(sta_ssid) > 0) {
        wifi_config_t sta_conf = {0};
        strlcpy((char *)sta_conf.sta.ssid,     sta_ssid, sizeof(sta_conf.sta.ssid));
        strlcpy((char *)sta_conf.sta.password,  sta_pass, sizeof(sta_conf.sta.password));
        sta_conf.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        sta_conf.sta.pmf_cfg.capable    = true;
        sta_conf.sta.pmf_cfg.required   = false;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_conf));
        ESP_LOGI(TAG, "STA credentials loaded for SSID: %s", sta_ssid);
    } else {
        ESP_LOGW(TAG, "No STA credentials in NVS — AP only mode");
    }

    s_initialized = true;
    ESP_LOGI(TAG, "WiFi manager initialized (AP: %s)", cfg->ap_ssid);
    return ESP_OK;
}

esp_err_t wifi_manager_start(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    esp_err_t ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Read AP IP */
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK) {
        snprintf(s_ap_ip, sizeof(s_ap_ip), IPSTR, IP2STR(&ip_info.ip));
    }

    ESP_LOGI(TAG, "WiFi started. AP IP: %s", s_ap_ip);
    return ESP_OK;
}

esp_err_t wifi_manager_stop(void)
{
    return esp_wifi_stop();
}

/* ── Credentials ──────────────────────────────────────────────────────────── */

esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password)
{
    if (!ssid) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(WIFI_NVS_NS, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) return ret;

    nvs_set_str(nvs, "ssid", ssid);
    nvs_set_str(nvs, "password", password ? password : "");
    ret = nvs_commit(nvs);
    nvs_close(nvs);

    ESP_LOGI(TAG, "WiFi credentials saved for SSID: %s", ssid);
    return ret;
}

esp_err_t wifi_manager_load_credentials(char *ssid, size_t ssid_len,
                                         char *pass, size_t pass_len)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(WIFI_NVS_NS, NVS_READONLY, &nvs);
    if (ret != ESP_OK) return ret;

    ret = nvs_get_str(nvs, "ssid", ssid, &ssid_len);
    if (ret == ESP_OK && pass != NULL) {
        nvs_get_str(nvs, "password", pass, &pass_len);
    }

    nvs_close(nvs);
    return ret;
}

esp_err_t wifi_manager_reconnect_sta(const char *ssid, const char *password)
{
    if (!ssid) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "Reconnecting STA to SSID: %s", ssid);

    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));

    wifi_config_t sta_conf = {0};
    strlcpy((char *)sta_conf.sta.ssid,     ssid,                     sizeof(sta_conf.sta.ssid));
    strlcpy((char *)sta_conf.sta.password,  password ? password : "", sizeof(sta_conf.sta.password));
    sta_conf.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    sta_conf.sta.pmf_cfg.capable    = true;
    sta_conf.sta.pmf_cfg.required   = false;

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &sta_conf);
    if (ret != ESP_OK) return ret;

    s_retry_num = 0;
    s_sta_state = WIFI_STA_CONNECTING;

    return esp_wifi_connect();
}

/* ── Status ───────────────────────────────────────────────────────────────── */

wifi_sta_state_t wifi_manager_get_sta_state(void)
{
    return s_sta_state;
}

const char *wifi_manager_get_sta_ip(void)
{
    return (s_sta_state == WIFI_STA_CONNECTED) ? s_sta_ip : NULL;
}

const char *wifi_manager_get_ap_ip(void)
{
    return s_ap_ip;
}

uint8_t wifi_manager_get_ap_client_count(void)
{
    wifi_sta_list_t sta_list = {0};
    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
        return (uint8_t)sta_list.num;
    }
    return 0;
}
