// File: components/wifi_provisioning/include/wifi_provisioning.h
/**
 * @file    wifi_provisioning.h
 * @brief   SoftAP-based provisioning entry points for pinMonitor.
 *
 * @details
 * This module coordinates provisioning:
 *  - Initializes system services (NVS, netif, event loop, Wi-Fi driver).
 *  - On @ref wifi_provisioning_start:
 *      * If Wi-Fi creds exist in NVS ("wifi_store": "ssid", "password"), attempts STA connect.
 *      * If missing or connect fails, starts SoftAP + provisioning web server.
 *
 * The web server (see web_server.h/.c) saves:
 *  - Wi-Fi creds to "wifi_store" ("ssid", "password")
 *  - MQTT settings to "mqtt_store" ("uri", "user", "pass")
 * and then reboots the device.
 */
#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize provisioning prerequisites (NVS, netif, event loop, Wi-Fi).
 * @return ESP_OK on success (idempotent; safe if some subsystems already exist).
 */
esp_err_t wifi_provisioning_init(void);

/**
 * @brief Start the provisioning flow.
 * @return
 *   - ESP_OK if connected with stored creds OR SoftAP server started successfully.
 *   - Error code on unrecoverable failure.
 */
esp_err_t wifi_provisioning_start(void);

/**
 * @brief Stop any provisioning services (e.g., web server) if you expose such a flow.
 * @note Currently not required in the happy path because the device reboots after save.
 */
static inline void wifi_provisioning_stop(void) { /* optional noop for now */ }

#ifdef __cplusplus
}
#endif
