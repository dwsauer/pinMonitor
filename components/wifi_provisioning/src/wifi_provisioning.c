#include <string.h>
#include "wifi_provisioning.h"
#include "web_server.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#define MAX_RETRY 5
static const char *TAG = "wifi_prov";
static int retry_count = 0;

static esp_err_t load_credentials(char *ssid, char *pass) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi_store", NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;

    size_t ssid_len = 32, pass_len = 64;
    err = nvs_get_str(nvs, "ssid", ssid, &ssid_len);
    if (err == ESP_OK) err = nvs_get_str(nvs, "pass", pass, &pass_len);

    nvs_close(nvs);
    return err;
}

static esp_err_t save_credentials(const char *ssid, const char *pass) {
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open("wifi_store", NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, "ssid", ssid));
    ESP_ERROR_CHECK(nvs_set_str(nvs, "pass", pass));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);
    return ESP_OK;
}

esp_err_t wifi_provisioning_init(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    return ESP_OK;
}

esp_err_t wifi_provisioning_start(void) {
    char ssid[32] = {0}, pass[64] = {0};
    if (load_credentials(ssid, pass) == ESP_OK) {
        ESP_LOGI(TAG, "Loaded SSID: %s", ssid);
        wifi_config_t wifi_config = {};
        strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
        strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_ERROR_CHECK(esp_wifi_connect());

        // Retry logic
        while (retry_count < MAX_RETRY) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                ESP_LOGI(TAG, "Connected to %s", ssid);
                return ESP_OK;
            }
            retry_count++;
        }

        ESP_LOGW(TAG, "Failed to connect. Entering provisioning mode...");
    }

    // Fallback to SoftAP + Web Server
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    wifi_config_t ap_config = {
        .ap = {
            .ssid = "ESP32_Setup",
            .ssid_len = strlen("ESP32_Setup"),
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN
        }
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    web_server_start(save_credentials);  // Callback to save creds
    return ESP_OK;
}

esp_err_t wifi_provisioning_stop(void) {
    web_server_stop();
    esp_wifi_stop();
    return ESP_OK;
}