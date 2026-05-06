idf_component_register(
    SRCS "web_server.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_http_server
        ota_manager
        wifi_manager
        boot_manager
        spiffs
        esp_partition
        app_update
        nvs_flash
        log
        esp_system
        esp_timer
        json
        esp_wifi
)
