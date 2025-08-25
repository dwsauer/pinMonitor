#include <string.h>
#include "wifi_provisioning.h"
#include "web_server.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "nvs.h"

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
 * @brief Erases stored Wi-Fi credentials from NVS.
 */
static void erase_wifi_credentials_nvs(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_WIFI, NVS_READWRITE, &nvs) == ESP_OK)
    {
        (void)nvs_erase_key(nvs, NVS_KEY_SSID);
        (void)nvs_erase_key(nvs, NVS_KEY_PASS);
        (void)nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "Erased Wi-Fi credentials from NVS");
    }
}
#endif

/**
 * @brief Saves Wi-Fi credentials (SSID and password) to NVS.
 */
static void save_credentials_nvs(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_WIFI, NVS_READWRITE, &nvs) != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs_open failed");
        return;
    }

    esp_err_t err = ESP_OK;
    err |= nvs_set_str(nvs, NVS_KEY_SSID, ssid);
    err |= nvs_set_str(nvs, NVS_KEY_PASS, pass);
    err |= nvs_commit(nvs);

    nvs_close(nvs);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save credentials: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Saved credentials: ssid=\"%s\" (len=%u)", ssid, (unsigned)strlen(ssid));
    }
}

/**
 * @brief Loads Wi-Fi credentials (SSID and password) from NVS.
 */
static esp_err_t load_credentials_nvs(char *ssid, size_t ssid_len,
                                      char *pass, size_t pass_len)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NS_WIFI, NVS_READONLY, &nvs);
    if (err != ESP_OK)
        return err;

    err = nvs_get_str(nvs, NVS_KEY_SSID, ssid, &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs, NVS_KEY_PASS, pass, &pass_len);
    }

    nvs_close(nvs);
    return err;
}

/**
 * @brief Initialize WiFi provisioning system.
 */
esp_err_t wifi_provisioning_init(void)
{
    // Fail-fast: system cannot continue without NVS, netif, event loop
    ESP_ERROR_CHECK(nvs_flash_init());
#ifdef ERASE_WIFI_CREDENTIALS_AT_STARTUP
    erase_wifi_credentials_nvs();
#endif
    ESP_ERROR_CHECK(esp_netif_init());

    // Event loop may already exist in some flows; ignore that case.
    esp_err_t ev = esp_event_loop_create_default();
    if (ev != ESP_OK && ev != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ev);
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    return ESP_OK;
}

/**
 * @brief Stops the SoftAP interface if running.
 */
static esp_err_t stop_softap_if_running(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK)
        return err;

    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA)
    {
        ESP_RETURN_ON_ERROR(esp_wifi_stop(), TAG, "Failed to stop WiFi");
        s_wifi_started = false;
    }
    return ESP_OK;
}

/**
 * @brief Starts SoftAP provisioning.
 */
static esp_err_t start_softap_provisioning(void)
{
    esp_netif_create_default_wifi_ap();

    wifi_config_t ap_cfg = {0};
    memcpy(ap_cfg.ap.ssid, "ESP32_Setup", sizeof("ESP32_Setup"));
    ap_cfg.ap.ssid_len      = strlen("ESP32_Setup");
    ap_cfg.ap.channel       = 1;
    ap_cfg.ap.max_connection= 4;
    ap_cfg.ap.authmode      = WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "Failed to set AP+STA mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "Failed to set AP config");

    if (!s_wifi_started)
    {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Failed to start WiFi");
        s_wifi_started = true;
    }

    ESP_LOGI(TAG, "Heap before web_server_start: %u", esp_get_free_heap_size());
    web_server_start(save_credentials_nvs);
    ESP_LOGI(TAG, "Heap after  web_server_start: %u", esp_get_free_heap_size());
    return ESP_OK;
}


/**
 * @brief Starts the Wi-Fi provisioning process.
 *
 * This function initializes and begins the Wi-Fi provisioning procedure,
 * allowing the device to be configured with Wi-Fi credentials.
 *
 * @return
 *     - ESP_OK: Success
 *     - Appropriate error code from esp_err_t in case of failure
 */
esp_err_t wifi_provisioning_start(void)
{
    char ssid[32] = {0}, pass[64] = {0};
    esp_err_t err = load_credentials_nvs(ssid, sizeof(ssid), pass, sizeof(pass));

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Loaded SSID from NVS: \"%s\"", ssid);

        wifi_config_t wifi_cfg = (wifi_config_t){0};
        strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid));
        strncpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password));

        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Failed to set STA mode");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "Failed to set STA config");

        if (!s_wifi_started) {
            ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Failed to start WiFi");
            s_wifi_started = true;
        }

        ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "Failed to connect");

        // Retry/poll loop to confirm connection
        retry_count = 0;
        while (retry_count < MAX_RETRY)
        {
            vTaskDelay(pdMS_TO_TICKS(2000));
            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
            {
                ESP_LOGI(TAG, "Connected to \"%s\"", ssid);
                return ESP_OK;
            }
            retry_count++;
            ESP_LOGW(TAG, "Connect retry %d/%d", retry_count, MAX_RETRY);
        }

        ESP_LOGW(TAG, "Failed to connect with stored credentials. Entering provisioning mode...");
        // fall through to SoftAP provisioning
    }

    // Fallback to SoftAP provisioning
    ESP_RETURN_ON_ERROR(start_softap_provisioning(), TAG, "SoftAP provisioning failed");
    return ESP_OK;
}
