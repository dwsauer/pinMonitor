// File: components/wifi_provisioning/include/web_server.h
/**
 * @file    web_server.h
 * @brief   Minimal provisioning web server for pinMonitor.
 *
 * @details
 * Exposes a tiny HTTP server that:
 *  - GET "/"    : renders a form showing scanned SSIDs and fields for Wi-Fi password
 *                 and MQTT broker settings (URI/username/password).
 *  - POST "/submit": writes credentials to NVS, then reboots.
 *
 * NVS namespaces & keys written by the server:
 *  - "wifi_store":  "ssid", "password"
 *  - "mqtt_store":  "uri", "user", "pass"
 *
 * Typical usage (during provisioning mode):
 * @code
 *   ESP_ERROR_CHECK(web_server_start());
 *   // ... user submits form ...
 *   // device reboots automatically after saving
 * @endcode
 */
#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Start the provisioning HTTP server (idempotent). */
esp_err_t web_server_start(void);

/** @brief Stop the provisioning HTTP server if it is running. */
void      web_server_stop(void);

#ifdef __cplusplus
}
#endif
