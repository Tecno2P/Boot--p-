idf_component_register(
    SRCS "boot_manager.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_system
        app_update
        esp_partition
        log
)
