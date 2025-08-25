/**
 * @file main.c
 * @brief PinMonitor application for ESP32 with Wi-Fi and MQTT integration.
 *
 * This application monitors GPIO pins with debounce logic and publishes pin events to an MQTT broker.
 * It initializes Wi-Fi in station mode, connects to a specified network, and starts an MQTT client.
 * GPIO pins are configured with interrupt and debounce handling, and their state changes are sent as MQTT messages.
 *
 * Features:
 * - Wi-Fi initialization and connection management.
 * - MQTT client setup and event publishing.
 * - GPIO pin monitoring with configurable debounce logic.
 * - Task-based event handling using FreeRTOS.
 *
 * Modules Used:
 * - ESP-IDF Wi-Fi, MQTT, GPIO, FreeRTOS, and timer APIs.
 * - Custom debounce and Wi-Fi manager modules.
 *
 * Configuration:
 * - Wi-Fi SSID and password are stored in NVS.
 * - MQTT broker URI, username, and password are hardcoded (demo).
 * - GPIO pins and debounce parameters are configurable.
 *
 * Usage:
 * - Deploy on ESP32 hardware.
 * - Ensure network and MQTT broker availability.
 * - Monitor MQTT topic for pin events.
 *
 * @author David Sauer
 * @date 8/18/2025
 */

// Git test 8/18/2025 1620

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_timer.h"

#include "debounce.h"
#include "wifi_manager.h"
#include "wifi_provisioning.h"
#include "app_shared.h"

static EventGroupHandle_t wifi_event_group;
static const char *TAG = "PinMonitor";

// ==== GLOBALS (single definitions; extern in main_priv.h) ====
QueueHandle_t gpio_event_queue = NULL;
esp_mqtt_client_handle_t mqtt_client = NULL;

void mqtt_app_start(void);
static void pin_monitor_init(void);
static void gpio_task(void *arg);

#define WIFI_CONNECTED_BIT BIT0
#define ESP_INTR_FLAG_DEFAULT 0

// ---- GPIO event handling task (publishes MQTT from main context) ----
static void gpio_task(void *arg)
{
    gpio_event_t evt;
    for (;;)
    {
        if (xQueueReceive(gpio_event_queue, &evt, portMAX_DELAY))
        {
            char msg[64];
            snprintf(msg, sizeof(msg),
                     "GPIO %d is now %s",
                     evt.pin, evt.level ? "HIGH" : "LOW");

            if (mqtt_client) {
                esp_mqtt_client_publish(mqtt_client, evt.topic ? evt.topic : "/pinMonitor/event",
                                        msg, 0, 1, 0);
            }
            ESP_LOGI(TAG, "Published: %s", msg);
        }
    }
}

// ---- Debounce + queue setup (GPIO handling happens in main) ----
static void pin_monitor_init(void)
{
    debounce_init();

    // Queue holds gpio_event_t sent by debounce.c timer callback
    gpio_event_queue = xQueueCreate(10, sizeof(gpio_event_t));
    xTaskCreate(gpio_task, "gpio_task", 4096, NULL, 10, NULL);

    debounce_config_t pin4_cfg = {
        .pin = GPIO_NUM_4,
        .intr_type = GPIO_INTR_POSEDGE,
        .pull_up = true,
        .debounce_time_us = 50000,
        .mqtt_topic = "/pinMonitor/gpio4"
    };
    debounce_register_pin(&pin4_cfg);

    debounce_config_t pin5_cfg = {
        .pin = GPIO_NUM_5,
        .intr_type = GPIO_INTR_NEGEDGE,
        .pull_up = true,
        .debounce_time_us = 75000,
        .mqtt_topic = "/pinMonitor/gpio5"
    };
    debounce_register_pin(&pin5_cfg);
}

// ---- Basic Wi-Fi station init using creds from NVS "wifi_store" ----
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta_ext(void)
{
    wifi_event_group = xEventGroupCreate();

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);

    // Read SSID and password from NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("wifi_store", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return;
    }

    char ssid[32] = {0};
    char password[64] = {0};
    size_t ssid_len = sizeof(ssid);
    size_t password_len = sizeof(password);

    err = nvs_get_str(nvs_handle, "ssid", ssid, &ssid_len);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "SSID not found in NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return;
    }

    // Unified on "password" key
    err = nvs_get_str(nvs_handle, "password", password, &password_len);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Password not found in NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return;
    }

    nvs_close(nvs_handle);

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    err = esp_wifi_start();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(err));
        return;
    }

    // Wait for connection
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);
    ESP_LOGI(TAG, "Connected to WiFi (NVS)");
}

void print_ip_info(esp_netif_t *netif)
{
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(netif, &ip_info);

    ESP_LOGI("NETIF", "IP Address: " IPSTR, IP2STR(&ip_info.ip));
    ESP_LOGI("NETIF", "Gateway:   " IPSTR, IP2STR(&ip_info.gw));
    ESP_LOGI("NETIF", "Netmask:   " IPSTR, IP2STR(&ip_info.netmask));
}

// ---- MQTT setup ----
void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address.uri = "mqtt://10.0.0.2:1883",
        },
        .credentials = {
            .username = "david1952",
            .authentication.password = "M9JP0Hz2iHaDbSX9CHn5",
        }
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_start(mqtt_client);
}

// ---- Entry point ----
void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Robust Wi-Fi credential check (unified keys)
    char ssid[32] = {0}, pass[64] = {0};
    bool creds_valid = false;
    nvs_handle_t nvs;
    esp_err_t nvs_err = nvs_open("wifi_store", NVS_READONLY, &nvs);
    if (nvs_err == ESP_OK)
    {
        size_t ssid_len = sizeof(ssid);
        size_t pass_len = sizeof(pass);
        esp_err_t ssid_err = nvs_get_str(nvs, "ssid", ssid, &ssid_len);
        // Use "password" key (matches wifi_init_sta_ext)
        esp_err_t pass_err = nvs_get_str(nvs, "password", pass, &pass_len);
        nvs_close(nvs);
        if (ssid_err == ESP_OK && pass_err == ESP_OK && ssid[0] != '\0')
        {
            creds_valid = true;
        }
    }

#ifdef ERASE_WIFI_CREDENTIALS_AT_STARTUP
    ESP_LOGW(TAG, "Wi-Fi credentials forced erase, starting provisioning...");
    ESP_ERROR_CHECK(wifi_provisioning_init());
    ESP_ERROR_CHECK(wifi_provisioning_start());
    ESP_LOGI(TAG, "Skipping normal Wi-Fi and MQTT since credentials are erased.");
    return;
#endif

    if (!creds_valid)
    {
        ESP_LOGW(TAG, "Wi-Fi credentials not found or invalid in NVS, starting provisioning...");
        ESP_ERROR_CHECK(wifi_provisioning_init());
        ESP_ERROR_CHECK(wifi_provisioning_start());
        ESP_LOGI(TAG, "Skipping normal Wi-Fi and MQTT since credentials are missing or invalid.");
        return;
    }
    else
    {
        ESP_LOGI(TAG, "Wi-Fi credentials found in NVS, starting normal Wi-Fi...");
        wifi_init_sta_ext(); // Use credentials from NVS
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            print_ip_info(netif);
        } else {
            ESP_LOGW(TAG, "WIFI_STA_DEF netif not found");
        }
        mqtt_app_start(); // Now safe to start MQTT
        pin_monitor_init(); // start debounce + queue + task
        ESP_LOGI(TAG, "PinMonitor started");
    }
}
