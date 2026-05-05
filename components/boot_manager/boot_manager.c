#include "boot_manager.h"

#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

static const char *TAG = "BOOT_MGR";

#define BOOT_STATE_NVS_NS   "boot_state"
#define KEY_FAIL_COUNT      "fail_count"
#define KEY_FORCE_BOOTMGR   "force_bm"
#define BOOT_MAX_FAILURES   3

static bool s_force_boot_manager = false;
static uint8_t s_fail_count = 0;

esp_err_t boot_manager_init(void)
{
    nvs_handle_t nvs;
    esp_err_t ret;

    ret = nvs_open(BOOT_STATE_NVS_NS, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Read current failure count */
    uint8_t fail_count = 0;
    ret = nvs_get_u8(nvs, KEY_FAIL_COUNT, &fail_count);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        fail_count = 0;
    } else if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Read fail_count error: %s", esp_err_to_name(ret));
        fail_count = 0;
    }

    /* Read force flag */
    uint8_t force_flag = 0;
    ret = nvs_get_u8(nvs, KEY_FORCE_BOOTMGR, &force_flag);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        force_flag = 0;
    }

    ESP_LOGI(TAG, "Boot fail count: %d, force flag: %d", fail_count, force_flag);

    /* Check OTA state - if pending verify, increment fail count */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            /* This partition is not yet confirmed valid */
            fail_count++;
            ESP_LOGW(TAG, "Partition pending verification, incrementing fail count to %d", fail_count);
            nvs_set_u8(nvs, KEY_FAIL_COUNT, fail_count);
            nvs_commit(nvs);
        }
    }

    s_fail_count = fail_count;

    /* If too many failures, force boot manager mode */
    if (fail_count >= BOOT_MAX_FAILURES || force_flag) {
        s_force_boot_manager = true;
        ESP_LOGW(TAG, "Forcing Boot Manager Mode (fail_count=%d, force_flag=%d)",
                 fail_count, force_flag);
        /* Clear flags so next reboot from Boot Manager works */
        nvs_set_u8(nvs, KEY_FAIL_COUNT, 0);
        nvs_set_u8(nvs, KEY_FORCE_BOOTMGR, 0);
        nvs_commit(nvs);
    }

    nvs_close(nvs);
    return ESP_OK;
}

bool boot_manager_is_forced(void)
{
    return s_force_boot_manager;
}

esp_err_t boot_manager_clear_counter(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(BOOT_STATE_NVS_NS, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }
    nvs_set_u8(nvs, KEY_FAIL_COUNT, 0);
    nvs_set_u8(nvs, KEY_FORCE_BOOTMGR, 0);
    ret = nvs_commit(nvs);
    nvs_close(nvs);
    s_fail_count = 0;
    ESP_LOGI(TAG, "Boot counter cleared");
    return ret;
}

uint8_t boot_manager_get_fail_count(void)
{
    return s_fail_count;
}
