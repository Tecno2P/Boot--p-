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
#include "driver/gpio.h"

#include "app_version.h"
#include "boot_manager.h"
#include "ota_manager.h"
#include "wifi_manager.h"
#include "web_server.h"

static const char *TAG = "MAIN";

#define BOOT_BUTTON_GPIO    GPIO_NUM_0
#define BOOT_BUTTON_HOLD_MS 500

/* Check if BOOT button is held at startup */
static bool boot_button_pressed(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    /* Sample button multiple times over hold period */
    int pressed_count = 0;
    const int samples = 10;
    for (int i = 0; i < samples; i++) {
        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            pressed_count++;
        }
        vTaskDelay(pdMS_TO_TICKS(BOOT_BUTTON_HOLD_MS / samples));
    }
    return (pressed_count >= (samples * 7 / 10)); /* 70% threshold */
}

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

    /* Initialize task watchdog */
    esp_task_wdt_config_t twdt_config = {
        .timeout_ms     = 30000,
        .idle_core_mask = 0,
        .trigger_panic  = true,
    };
    ESP_ERROR_CHECK(esp_task_wdt_init(&twdt_config));

    /* Initialize boot manager (handles boot loop detection + OTA marking) */
    ret = boot_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Boot manager init failed: %s", esp_err_to_name(ret));
    }

    /* Determine boot mode */
    bool force_boot_mgr = boot_manager_is_forced();
    bool btn_pressed    = boot_button_pressed();

    if (btn_pressed) {
        ESP_LOGI(TAG, "BOOT button held — entering Boot Manager Mode");
    }
    if (force_boot_mgr) {
        ESP_LOGW(TAG, "Boot loop detected — forcing Boot Manager Mode");
    }

    if (btn_pressed || force_boot_mgr) {
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

        /* Boot Manager runs indefinitely — reset to reboot normally */
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

        /* ── USER APPLICATION STARTS HERE ──
         * In a real product, initialize your application subsystems.
         * This example blinks an LED and logs status.
         */
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
