#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize boot manager.
 *
 * Must be called early in app_main.
 * Increments boot attempt counter in NVS.
 * If counter >= BOOT_MAX_FAILURES, sets forced boot-manager flag.
 */
esp_err_t boot_manager_init(void);

/**
 * @brief Returns true if Boot Manager Mode should be forced
 *        due to repeated boot failures.
 */
bool boot_manager_is_forced(void);

/**
 * @brief Clear the boot failure counter (call on successful app startup).
 */
esp_err_t boot_manager_clear_counter(void);

/**
 * @brief Get current boot failure count.
 */
uint8_t boot_manager_get_fail_count(void);

#ifdef __cplusplus
}
#endif
