#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Check if Wi-Fi credentials are stored
 *
 * @return true if credentials exist, false otherwise
 */
bool wifi_credentials_exist(void);

/**
 * @brief Load stored Wi-Fi credentials
 *
 * @param ssid Buffer to hold SSID
 * @param pass Buffer to hold password
 * @return ESP_OK on success, ESP_FAIL otherwise
 */
esp_err_t wifi_credentials_load(char *ssid, char *pass);

/**
 * @brief Save Wi-Fi credentials to NVS
 *
 * @param ssid SSID string
 * @param pass Password string
 * @return ESP_OK on success
 */
esp_err_t wifi_credentials_save(const char *ssid, const char *pass);

/**
 * @brief Clear stored Wi-Fi credentials
 *
 * @return ESP_OK on success
 */
esp_err_t wifi_credentials_clear(void);

#ifdef __cplusplus
}
#endif