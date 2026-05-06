idf_component_register(
    SRCS "ota_manager.c"
    INCLUDE_DIRS "."
    REQUIRES
        app_update
        esp_http_client
        esp_https_ota
        esp_partition
        nvs_flash
        log
        bootloader_support
        mbedtls
)
