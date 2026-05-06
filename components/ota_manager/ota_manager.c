#include "ota_manager.h"

#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

static const char *TAG = "OTA_MGR";

/* Upload state */
static esp_ota_handle_t  s_ota_handle   = 0;
static const esp_partition_t *s_ota_part = NULL;
static size_t             s_written      = 0;
static size_t             s_total        = 0;
static bool               s_upload_active = false;

/* ── Mark Valid ───────────────────────────────────────────────────────────── */

esp_err_t ota_manager_mark_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    esp_err_t ret = esp_ota_get_state_partition(running, &state);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Could not read OTA state: %s", esp_err_to_name(ret));
        return ret;
    }
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Marking OTA image valid on partition: %s", running->label);
        return esp_ota_mark_app_valid_cancel_rollback();
    }
    ESP_LOGI(TAG, "OTA state already valid (%d)", state);
    return ESP_OK;
}

/* ── OTA from URL ─────────────────────────────────────────────────────────── */

esp_err_t ota_manager_update_from_url(const char *url, ota_progress_cb_t progress)
{
    if (!url || strlen(url) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "OTA from URL: %s", url);

    esp_http_client_config_t http_cfg = {
        .url                = url,
        .timeout_ms         = 15000,
        .keep_alive_enable  = true,
        .skip_cert_common_name_check = false,
        .crt_bundle_attach  = esp_crt_bundle_attach,
        .buffer_size        = 4096,
        .buffer_size_tx     = 1024,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config       = &http_cfg,
        .http_client_init_cb = NULL,
        .bulk_flash_erase  = false,
        .partial_http_download = false,
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t ret = esp_https_ota_begin(&ota_cfg, &ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Get image size for progress reporting */
    esp_app_desc_t app_desc;
    ret = esp_https_ota_get_img_desc(ota_handle, &app_desc);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "New firmware: %s v%s", app_desc.project_name, app_desc.version);
    }

    size_t image_size = esp_https_ota_get_image_size(ota_handle);
    size_t written = 0;

    while (1) {
        ret = esp_https_ota_perform(ota_handle);
        if (ret == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            written = esp_https_ota_get_image_len_read(ota_handle);
            if (progress) {
                progress(written, image_size);
            }
            continue;
        }
        break;
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA perform failed: %s", esp_err_to_name(ret));
        esp_https_ota_abort(ota_handle);
        return ret;
    }

    if (!esp_https_ota_is_complete_data_received(ota_handle)) {
        ESP_LOGE(TAG, "OTA: incomplete data received");
        esp_https_ota_abort(ota_handle);
        return ESP_FAIL;
    }

    ret = esp_https_ota_finish(ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "OTA from URL complete. Reboot to apply.");
    return ESP_OK;
}

/* ── Browser Upload (chunked) ─────────────────────────────────────────────── */

esp_err_t ota_manager_begin_upload(size_t total_size)
{
    if (s_upload_active) {
        ESP_LOGW(TAG, "Upload already in progress — aborting previous");
        ota_manager_abort_upload();
    }

    /* Find the next OTA partition (not currently running) */
    const esp_partition_t *running = esp_ota_get_running_partition();
    s_ota_part = esp_ota_get_next_update_partition(NULL);

    if (s_ota_part == NULL) {
        ESP_LOGE(TAG, "No OTA partition available");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA upload target: %s (offset 0x%08x, size %u)",
             s_ota_part->label,
             (unsigned)s_ota_part->address,
             (unsigned)s_ota_part->size);

    if (running->address == s_ota_part->address) {
        ESP_LOGE(TAG, "Target partition is currently running — cannot overwrite");
        return ESP_FAIL;
    }

    esp_err_t ret = esp_ota_begin(s_ota_part, OTA_WITH_SEQUENTIAL_WRITES, &s_ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_written = 0;
    s_total = total_size;
    s_upload_active = true;
    ESP_LOGI(TAG, "OTA upload started, expected size: %zu bytes", total_size);
    return ESP_OK;
}

esp_err_t ota_manager_write_chunk(const uint8_t *data, size_t len)
{
    if (!s_upload_active || s_ota_handle == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_ota_write(s_ota_handle, data, len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA write failed at offset %zu: %s", s_written, esp_err_to_name(ret));
        ota_manager_abort_upload();
        return ret;
    }

    s_written += len;
    if (s_total > 0 && (s_written % (64 * 1024) == 0)) {
        ESP_LOGI(TAG, "OTA written: %zu / %zu bytes (%.1f%%)",
                 s_written, s_total,
                 s_total > 0 ? (float)s_written * 100.0f / (float)s_total : 0.0f);
    }
    return ESP_OK;
}

esp_err_t ota_manager_end_upload(void)
{
    if (!s_upload_active || s_ota_handle == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "OTA upload complete, total written: %zu bytes", s_written);

    esp_err_t ret = esp_ota_end(s_ota_handle);
    s_ota_handle = 0;
    s_upload_active = false;

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Validate the image */
    esp_app_desc_t new_desc;
    ret = esp_ota_get_partition_description(s_ota_part, &new_desc);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Validated firmware: %s v%s built %s %s",
                 new_desc.project_name, new_desc.version,
                 new_desc.date, new_desc.time);
    } else {
        ESP_LOGW(TAG, "Could not read new firmware description: %s", esp_err_to_name(ret));
    }

    /* Set boot partition */
    ret = esp_ota_set_boot_partition(s_ota_part);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Boot partition set to %s. Reboot to apply.", s_ota_part->label);
    s_ota_part = NULL;
    return ESP_OK;
}

void ota_manager_abort_upload(void)
{
    if (s_ota_handle != 0) {
        esp_ota_abort(s_ota_handle);
        s_ota_handle = 0;
    }
    s_ota_part = NULL;
    s_upload_active = false;
    s_written = 0;
    s_total = 0;
    ESP_LOGW(TAG, "OTA upload aborted");
}

/* ── Partition Switch ─────────────────────────────────────────────────────── */

esp_err_t ota_manager_switch_partition(void)
{
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (next == NULL) {
        ESP_LOGE(TAG, "No alternate OTA partition found");
        return ESP_FAIL;
    }

    /* Verify that next partition has a valid image */
    esp_ota_img_states_t state;
    esp_err_t ret = esp_ota_get_state_partition(next, &state);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Cannot read state of partition %s: %s", next->label, esp_err_to_name(ret));
    }

    if (state == ESP_OTA_IMG_INVALID || state == ESP_OTA_IMG_ABORTED) {
        ESP_LOGE(TAG, "Target partition %s has invalid image (state=%d)", next->label, state);
        return ESP_FAIL;
    }

    ret = esp_ota_set_boot_partition(next);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to switch partition: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Switched boot partition to: %s", next->label);
    return ESP_OK;
}

/* ── Info ─────────────────────────────────────────────────────────────────── */

const char *ota_manager_get_running_partition_label(void)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    return p ? p->label : "unknown";
}

const char *ota_manager_get_next_partition_label(void)
{
    const esp_partition_t *p = esp_ota_get_next_update_partition(NULL);
    return p ? p->label : "none";
}

esp_err_t ota_manager_get_app_desc(esp_app_desc_t *desc)
{
    if (!desc) return ESP_ERR_INVALID_ARG;
    const esp_partition_t *running = esp_ota_get_running_partition();
    return esp_ota_get_partition_description(running, desc);
}

bool ota_manager_next_partition_valid(void)
{
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (!next) return false;

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(next, &state) != ESP_OK) {
        return false;
    }
    return (state == ESP_OTA_IMG_VALID || state == ESP_OTA_IMG_PENDING_VERIFY);
}
