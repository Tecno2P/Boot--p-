#pragma once
#include <stddef.h>

#include "esp_err.h"
#include "esp_wifi.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_SSID_MAX_LEN     32
#define WIFI_PASSWORD_MAX_LEN 64
#define WIFI_NVS_NS           "wifi_config"
#define WIFI_STA_MAX_RETRY    5

typedef struct {
    char     ap_ssid[WIFI_SSID_MAX_LEN];
    char     ap_password[WIFI_PASSWORD_MAX_LEN];
    uint8_t  ap_channel;
    uint8_t  ap_max_conn;
} wifi_manager_config_t;

typedef enum {
    WIFI_STA_DISCONNECTED = 0,
    WIFI_STA_CONNECTING,
    WIFI_STA_CONNECTED,
    WIFI_STA_FAILED,
} wifi_sta_state_t;

/**
 * @brief Initialize WiFi in AP+STA mode.
 * @param cfg  AP configuration (STA creds loaded from NVS)
 */
esp_err_t wifi_manager_init(const wifi_manager_config_t *cfg);

/**
 * @brief Start WiFi (AP always, STA if credentials exist in NVS).
 */
esp_err_t wifi_manager_start(void);

/**
 * @brief Stop WiFi.
 */
esp_err_t wifi_manager_stop(void);

/**
 * @brief Save STA credentials to NVS.
 */
esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password);

/**
 * @brief Load STA credentials from NVS.
 * @return ESP_OK if credentials exist, ESP_ERR_NVS_NOT_FOUND otherwise.
 */
esp_err_t wifi_manager_load_credentials(char *ssid, size_t ssid_len,
                                         char *pass, size_t pass_len);

/**
 * @brief Reconnect STA with new credentials (loaded from NVS or provided).
 */
esp_err_t wifi_manager_reconnect_sta(const char *ssid, const char *password);

/**
 * @brief Get current STA connection state.
 */
wifi_sta_state_t wifi_manager_get_sta_state(void);

/**
 * @brief Get STA IP address string (or NULL if not connected).
 */
const char *wifi_manager_get_sta_ip(void);

/**
 * @brief Get AP IP address string.
 */
const char *wifi_manager_get_ap_ip(void);

/**
 * @brief Get number of connected AP clients.
 */
uint8_t wifi_manager_get_ap_client_count(void);

#ifdef __cplusplus
}
#endif
