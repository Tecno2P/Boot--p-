#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_task_wdt.h"
#include "esp_ota_ops.h"

#include "app_version.h"
#include "boot_manager.h"
#include "ota_manager.h"
#include "wifi_manager.h"
#include "web_server.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "=== ESP32 Dual Boot Manager v%s ===", APP_VERSION);
    ESP_LOGI(TAG, "IDF version: %s", esp_get_idf_version());

    /* Initialize NVS */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Initialize event loop */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Initialize (or reconfigure) task watchdog.
     * In ESP-IDF v5.x the TWDT may already be initialized by the system
     * before app_main() runs, so fall back to reconfigure in that case. */
    esp_task_wdt_config_t twdt_config = {
        .timeout_ms     = 30000,
        .idle_core_mask = 0,
        .trigger_panic  = true,
    };
    ret = esp_task_wdt_init(&twdt_config);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "TWDT already initialized, reconfiguring...");
        ESP_ERROR_CHECK(esp_task_wdt_reconfigure(&twdt_config));
    } else {
        ESP_ERROR_CHECK(ret);
    }

    /* Initialize boot manager (handles boot loop detection + OTA marking) */
    ret = boot_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Boot manager init failed: %s", esp_err_to_name(ret));
    }

    /* Determine boot mode — boot loop detection only, no GPIO */
    bool force_boot_mgr = boot_manager_is_forced();

    if (force_boot_mgr) {
        ESP_LOGW(TAG, "Boot loop detected — forcing Boot Manager Mode");

        /* ── BOOT MANAGER MODE ── */
        ESP_LOGI(TAG, "Starting Boot Manager...");

        /* Mount SPIFFS for web UI assets */
        ret = web_server_mount_spiffs();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "SPIFFS mount failed (%s) — web UI may not load", esp_err_to_name(ret));
        }

        /* Start WiFi in AP+STA mode */
        wifi_manager_config_t wifi_cfg = {
            .ap_ssid     = "ESP32-BootManager",
            .ap_password = "12345678",
            .ap_channel  = 1,
            .ap_max_conn = 4,
        };
        ESP_ERROR_CHECK(wifi_manager_init(&wifi_cfg));
        ESP_ERROR_CHECK(wifi_manager_start());

        /* Start HTTP web server */
        ESP_ERROR_CHECK(web_server_start());

        ESP_LOGI(TAG, "Boot Manager running. Connect to SSID: ESP32-BootManager / 192.168.4.1");

        /* Keep alive — watchdog fed by web server task */
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(10000));
            ESP_LOGI(TAG, "Boot Manager alive. Free heap: %u bytes",
                     (unsigned)esp_get_free_heap_size());
        }

    } else {
        /* ── NORMAL APPLICATION MODE ── */
        ESP_LOGI(TAG, "Normal boot — starting application");

        /* Mark current firmware as valid (cancels rollback timer) */
        ret = ota_manager_mark_valid();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Could not mark OTA valid: %s", esp_err_to_name(ret));
        }

        /* Clear boot loop counter on successful startup */
        boot_manager_clear_counter();

        ESP_LOGI(TAG, "Application running. Firmware version: %s", APP_VERSION);

        const esp_partition_t *running = esp_ota_get_running_partition();
        if (running) {
            ESP_LOGI(TAG, "Running from partition: %s (offset 0x%08x)",
                     running->label, (unsigned)running->address);
        }

        uint32_t blink_count = 0;
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            blink_count++;
            ESP_LOGI(TAG, "Heartbeat #%u | Heap: %u free",
                     (unsigned)blink_count,
                     (unsigned)esp_get_free_heap_size());
        }
    }
}
