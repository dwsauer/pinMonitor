// File: components/wifi_provisioning/wifi_provisioning.c
/**
 * @file    wifi_provisioning.c
 * @brief   SoftAP-based provisioning for pinMonitor (Wi-Fi + web form).
 *
 * @details
 * - If Wi-Fi credentials are present in NVS ("wifi_store": "ssid", "password"),
 *   try STA mode and connect.
 * - If missing or connection fails after a few retries, start SoftAP + HTTP server
 *   so the user can submit Wi-Fi + MQTT settings (handled in web_server.c).
 *
 * Notes:
 * - This module owns Wi-Fi bring-up while provisioning is active.
 * - The HTTP server writes Wi-Fi to "wifi_store" and MQTT to "mqtt_store", then reboots.
 * - Keep initialization idempotent: NVS / netif / event loop may have been created already.
 */

#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"
#include "wifi_provisioning.h"
#include "web_server.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_heap_caps.h"   // esp_get_free_heap_size()

#define MAX_RETRY        5

// NVS namespace and keys (MUST match main.c expectations)
#define NVS_NS_WIFI      "wifi_store"
#define NVS_KEY_SSID     "ssid"
#define NVS_KEY_PASS     "password"

static const char *TAG = "wifi_prov";
static int  retry_count = 0;
static bool s_wifi_started = false;

#ifdef ERASE_WIFI_CREDENTIALS_AT_STARTUP
/**
 * @brief Erase stored Wi-Fi credentials from NVS (build-time opt-in).
 */
static void erase_wifi_credentials_nvs(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_WIFI, NVS_READWRITE, &nvs) == ESP_OK) {
        (void)nvs_erase_key(nvs, NVS_KEY_SSID);
        (void)nvs_erase_key(nvs, NVS_KEY_PASS);
        (void)nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "Erased Wi-Fi credentials from NVS");
    }
}
#endif

#if 0
/**
 * @brief Save Wi-Fi credentials (SSID + password) to NVS.
 */
static void save_credentials_nvs(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_WIFI, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed");
        return;
    }

    esp_err_t err = ESP_OK;
    err |= nvs_set_str(nvs, NVS_KEY_SSID, ssid ? ssid : "");
    err |= nvs_set_str(nvs, NVS_KEY_PASS, pass ? pass : "");
    err |= nvs_commit(nvs);
    nvs_close(nvs);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save credentials: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Saved credentials: ssid=\"%s\" (len=%u)", ssid ? ssid : "", (unsigned)(ssid ? strlen(ssid) : 0));
    }
}
#endif

/**
 * @brief Load Wi-Fi credentials (SSID + password) from NVS.
 *
 * @param[out] ssid  Buffer for SSID
 * @param[in]  ssid_len Length of SSID buffer
 * @param[out] pass  Buffer for password
 * @param[in]  pass_len Length of password buffer
 * @return ESP_OK if both SSID and password were read successfully.
 */
static esp_err_t load_credentials_nvs(char *ssid, size_t ssid_len,
                                      char *pass, size_t pass_len)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NS_WIFI, NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;

    err = nvs_get_str(nvs, NVS_KEY_SSID, ssid, &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs, NVS_KEY_PASS, pass, &pass_len);
    }

    nvs_close(nvs);
    return err;
}

/**
 * @brief Initialize provisioning: NVS, netif, default event loop, Wi-Fi driver.
 *
 * @note Safe to call even if some subsystems are already initialized.
 */
esp_err_t wifi_provisioning_init(void)
{
    // Fail-fast: system cannot continue without NVS / netif / event loop.
    ESP_ERROR_CHECK(nvs_flash_init());

#ifdef ERASE_WIFI_CREDENTIALS_AT_STARTUP
    erase_wifi_credentials_nvs();
#endif

    ESP_ERROR_CHECK(esp_netif_init());
    // Event loop may already exist; ignore that case.
    esp_err_t ev = esp_event_loop_create_default();
    if (ev != ESP_OK && ev != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ev);
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    return ESP_OK;
}

#if 0
/**
 * @brief Stop SoftAP interface if running.
 *
 * @return ESP_OK or last error.
 *
 * @note Currently unused; keep for future flows that need to toggle AP/STA.
 */
static esp_err_t stop_softap_if_running(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK) return err;

    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
        ESP_RETURN_ON_ERROR(esp_wifi_stop(), TAG, "Failed to stop Wi-Fi");
        s_wifi_started = false;
    }
    return ESP_OK;
}
#endif

/**
 * @brief Start SoftAP + HTTP provisioning server.
 *
 * - Creates default AP netif.
 * - Sets open SSID "ESP32_Setup" (customize as needed).
 * - Sets mode AP+STA (so you can also scan STA side if desired).
 * - Starts Wi-Fi if not already started.
 * - Starts the provisioning web server (saves Wi-Fi + MQTT and reboots).
 */
static esp_err_t start_softap_provisioning(void)
{
    // Create default AP netif (DHCP server is started internally by esp_netif defaults)
    esp_netif_create_default_wifi_ap();

    // Build a unique SSID from the SoftAP MAC (last three bytes)
    uint8_t mac[6] = {0};
    // You can also use WIFI_IF_AP with esp_wifi_get_mac(), but esp_read_mac works before Wi-Fi start.
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));

    char ap_ssid[32];
    // "ESP32_Setup_" + 6 hex chars = 13 + 6 = 19 (+NUL) < 32 bytes — safe
    snprintf(ap_ssid, sizeof(ap_ssid), "ESP32_Setup_%02X%02X%02X", mac[3], mac[4], mac[5]);

    wifi_config_t ap_cfg = (wifi_config_t){0};
    // Copy SSID into config (NUL-terminated)
    snprintf((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), "%s", ap_ssid);
    ap_cfg.ap.ssid_len        = strlen((const char *)ap_cfg.ap.ssid);
    ap_cfg.ap.channel         = 1;
    ap_cfg.ap.max_connection  = 4;
    ap_cfg.ap.authmode        = WIFI_AUTH_OPEN;   // change to WPA2 if you want an AP password
    // ap_cfg.ap.password[...] // set and switch authmode if you want a protected AP

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "Failed to set AP+STA mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "Failed to set AP config");

    if (!s_wifi_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Failed to start Wi-Fi");
        s_wifi_started = true;
    }

    ESP_LOGI(TAG, "SoftAP SSID: %s", (const char*)ap_cfg.ap.ssid);
    ESP_LOGI(TAG, "Heap before web_server_start: %u", (unsigned)esp_get_free_heap_size());
    ESP_ERROR_CHECK(web_server_start());   // serves Wi-Fi + MQTT form; saves to NVS; reboots
    ESP_LOGI(TAG, "Heap after  web_server_start: %u", (unsigned)esp_get_free_heap_size());
    return ESP_OK;
}

/**
 * @brief Start provisioning flow: try stored creds; if fail, start SoftAP provisioning.
 *
 * @return ESP_OK if connected using stored creds OR SoftAP server started successfully.
 */
esp_err_t wifi_provisioning_start(void)
{
    char ssid[32] = {0}, pass[64] = {0};
    esp_err_t err = load_credentials_nvs(ssid, sizeof(ssid), pass, sizeof(pass));

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Loaded SSID from NVS: \"%s\"", ssid);

        wifi_config_t wifi_cfg = (wifi_config_t){0};
        strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid));
        strncpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password));

        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Failed to set STA mode");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "Failed to set STA config");

        if (!s_wifi_started) {
            ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Failed to start Wi-Fi");
            s_wifi_started = true;
        }

        ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "Failed to connect");

        // Simple retry/poll loop to confirm connection
        retry_count = 0;
        while (retry_count < MAX_RETRY) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                ESP_LOGI(TAG, "Connected to \"%s\"", ssid);
                return ESP_OK;
            }
            retry_count++;
            ESP_LOGW(TAG, "Connect retry %d/%d", retry_count, MAX_RETRY);
        }

        ESP_LOGW(TAG, "Failed to connect with stored credentials. Entering provisioning mode…");
        // fall through to SoftAP provisioning
    }

    // Fallback to SoftAP provisioning
    ESP_RETURN_ON_ERROR(start_softap_provisioning(), TAG, "SoftAP provisioning failed");
    return ESP_OK;
}
