#include <string.h>
#include "wifi_provisioning.h"
#include "web_server.h"

#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "nvs.h"

#define MAX_RETRY 5
#define NVS_NS_WIFI "wifi_store"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASS "pass"

static const char *TAG = "wifi_prov";
static int retry_count = 0;
static bool s_wifi_started = false;


#ifdef ERASE_WIFI_CREDENTIALS_AT_STARTUP
static void erase_wifi_credentials_nvs(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_WIFI, NVS_READWRITE, &nvs) == ESP_OK)
    {
        nvs_erase_key(nvs, NVS_KEY_SSID);
        nvs_erase_key(nvs, NVS_KEY_PASS);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "Erased Wi-Fi credentials from NVS");
    }
}
#endif

/* -------------------------------------------------------------------------- */
/*                              NVS Credential IO                             */
/* -------------------------------------------------------------------------- */
static void save_credentials_nvs(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_WIFI, NVS_READWRITE, &nvs) != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs_open failed");
        return;
    }

    if (nvs_set_str(nvs, NVS_KEY_SSID, ssid) != ESP_OK ||
        nvs_set_str(nvs, NVS_KEY_PASS, pass) != ESP_OK ||
        nvs_commit(nvs) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to save credentials");
    }

    nvs_close(nvs);
}

static esp_err_t load_credentials_nvs(char *ssid, size_t ssid_len,
                                      char *pass, size_t pass_len)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NS_WIFI, NVS_READONLY, &nvs);
    if (err != ESP_OK)
        return err;

    err = nvs_get_str(nvs, NVS_KEY_SSID, ssid, &ssid_len);
    if (err == ESP_OK)
    {
        err = nvs_get_str(nvs, NVS_KEY_PASS, pass, &pass_len);
    }

    nvs_close(nvs);
    return err;
}

/* -------------------------------------------------------------------------- */
/*                             WiFi Provisioning                              */
/* -------------------------------------------------------------------------- */
esp_err_t wifi_provisioning_init(void)
{
    // Fail-fast: system cannot continue without NVS, netif, event loop
    ESP_ERROR_CHECK(nvs_flash_init());
#ifdef ERASE_WIFI_CREDENTIALS_AT_STARTUP
    erase_wifi_credentials_nvs();
#endif
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_ERROR_CHECK(err);
    }
    ESP_ERROR_CHECK(esp_wifi_init(&(wifi_init_config_t)WIFI_INIT_CONFIG_DEFAULT()));

    return ESP_OK;
}

static esp_err_t stop_softap_if_running(void)
{
    wifi_mode_t mode;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK)
        return err;

    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA)
    {
        ESP_RETURN_ON_ERROR(esp_wifi_stop(), TAG, "Failed to stop SoftAP");
        s_wifi_started = false;
    }
    return ESP_OK;
}

static esp_err_t start_softap_provisioning(void)
{
    esp_netif_create_default_wifi_ap();

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = "ESP32_Setup",
            .ssid_len = strlen("ESP32_Setup"),
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN}};

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "Failed to set AP+STA mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "Failed to set AP config");

    if (!s_wifi_started)
    {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Failed to start WiFi");
        s_wifi_started = true;
    }

    ESP_LOGI(TAG, "Heap before web_server_start: %u", esp_get_free_heap_size());
    web_server_start(save_credentials_nvs);
    ESP_LOGI(TAG, "Heap after web_server_start: %u", esp_get_free_heap_size());
    return ESP_OK;
}

esp_err_t wifi_provisioning_start(void)
{
    char ssid[32] = {0}, pass[64] = {0};
    esp_err_t err = load_credentials_nvs(ssid, sizeof(ssid), pass, sizeof(pass));

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Loaded SSID: %s", ssid);

        wifi_config_t wifi_cfg = {};
        strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid));
        strncpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password));

        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Failed to set STA mode");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "Failed to set STA config");
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Failed to start WiFi");
        s_wifi_started = true;

        ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "Failed to connect");

        // Retry loop
        retry_count = 0;
        while (retry_count < MAX_RETRY)
        {
            vTaskDelay(pdMS_TO_TICKS(2000));
            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
            {
                ESP_LOGI(TAG, "Connected to %s", ssid);
                return ESP_OK;
            }
            retry_count++;
        }

        ESP_LOGW(TAG, "Failed to connect. Entering provisioning mode...");
    }

    // Fallback to SoftAP provisioning
    ESP_RETURN_ON_ERROR(start_softap_provisioning(), TAG, "SoftAP provisioning failed");
    return ESP_OK;
}

esp_err_t wifi_provisioning_stop(void)
{
    web_server_stop();
    ESP_RETURN_ON_ERROR(stop_softap_if_running(), TAG, "Failed to stop SoftAP");

    if (s_wifi_started)
    {
        ESP_RETURN_ON_ERROR(esp_wifi_stop(), TAG, "Failed to stop WiFi");
        s_wifi_started = false;
    }
    return ESP_OK;
}
