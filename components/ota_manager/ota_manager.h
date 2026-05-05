#pragma once

#include "esp_err.h"
#include "esp_ota_ops.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** OTA progress callback type */
typedef void (*ota_progress_cb_t)(size_t written, size_t total);

/**
 * @brief Mark current running firmware as valid.
 *        Cancels rollback. Call after successful application init.
 */
esp_err_t ota_manager_mark_valid(void);

/**
 * @brief Perform OTA update from HTTP/HTTPS URL.
 *
 * @param url        Firmware URL (http:// or https://)
 * @param progress   Optional progress callback (can be NULL)
 * @return ESP_OK on success — caller must reboot after this returns.
 */
esp_err_t ota_manager_update_from_url(const char *url, ota_progress_cb_t progress);

/**
 * @brief Begin OTA upload from browser (chunk-based).
 *        Call ota_manager_write_chunk() repeatedly, then ota_manager_end_upload().
 *
 * @return ESP_OK on success
 */
esp_err_t ota_manager_begin_upload(size_t total_size);

/**
 * @brief Write a chunk of firmware data during browser upload.
 */
esp_err_t ota_manager_write_chunk(const uint8_t *data, size_t len);

/**
 * @brief Finalize browser-based OTA upload.
 *        Validates image and sets boot partition.
 * @return ESP_OK on success — caller must reboot.
 */
esp_err_t ota_manager_end_upload(void);

/**
 * @brief Abort an in-progress browser upload.
 */
void ota_manager_abort_upload(void);

/**
 * @brief Switch boot partition to the other OTA slot.
 *        Does not reboot — caller must reboot after.
 */
esp_err_t ota_manager_switch_partition(void);

/**
 * @brief Get label of currently running partition.
 */
const char *ota_manager_get_running_partition_label(void);

/**
 * @brief Get label of the next (standby) OTA partition.
 */
const char *ota_manager_get_next_partition_label(void);

/**
 * @brief Get firmware description of running partition.
 */
esp_err_t ota_manager_get_app_desc(esp_app_desc_t *desc);

/**
 * @brief Check if next partition has valid firmware.
 */
bool ota_manager_next_partition_valid(void);

#ifdef __cplusplus
}
#endif
