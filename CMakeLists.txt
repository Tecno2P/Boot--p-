cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_SOURCE_DIR}/components/boot_manager"
    "${CMAKE_SOURCE_DIR}/components/ota_manager"
    "${CMAKE_SOURCE_DIR}/components/wifi_manager"
    "${CMAKE_SOURCE_DIR}/components/web_server"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_boot_manager)

# ── SPIFFS image ─────────────────────────────────────────────────────────────
# Builds spiffs.bin from web_ui/ and flashes it alongside the application.
spiffs_create_partition_image(
    spiffs
    "${CMAKE_SOURCE_DIR}/web_ui"
    FLASH_IN_PROJECT
)
