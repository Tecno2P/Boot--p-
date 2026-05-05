#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Mount SPIFFS filesystem for web UI assets.
 */
esp_err_t web_server_mount_spiffs(void);

/**
 * @brief Start the HTTP web server.
 *        WiFi must be started before calling this.
 */
esp_err_t web_server_start(void);

/**
 * @brief Stop the HTTP web server.
 */
esp_err_t web_server_stop(void);

#ifdef __cplusplus
}
#endif
