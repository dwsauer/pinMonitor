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
 * - Wi-Fi SSID and password are hardcoded for demonstration.
 * - MQTT broker URI, username, and password are hardcoded.
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
#include "main_priv.h"

static EventGroupHandle_t wifi_event_group;
static const char *TAG = "PinMonitor";
static QueueHandle_t gpio_event_queue = NULL;
esp_mqtt_client_handle_t mqtt_client = NULL;

void mqtt_app_start(void);
void pin_monitor_init(void);
void gpio_task(void *arg);

#define WIFI_CONNECTED_BIT BIT0
#define GPIO_INPUT_PIN GPIO_NUM_4
#define ESP_INTR_FLAG_DEFAULT 0
#define DEBOUNCE_TIME_US 50000 // 50 ms

// Dummy implementation to resolve implicit declaration error.
// Replace with your actual pin monitor initialization logic.
void pin_monitor_init(void)
{
    debounce_init();

    gpio_event_queue = xQueueCreate(10, sizeof(uint32_t));
    xTaskCreate(gpio_task, "gpio_task", 2048, NULL, 10, NULL);

    debounce_config_t pin4_cfg = {
        .pin = GPIO_NUM_4,
        .intr_type = GPIO_INTR_POSEDGE,
        .pull_up = true,
        .debounce_time_us = 50000,
        .mqtt_topic = "/pinMonitor/gpio4"};
    debounce_register_pin(&pin4_cfg);

    debounce_config_t pin5_cfg = {
        .pin = GPIO_NUM_5,
        .intr_type = GPIO_INTR_NEGEDGE,
        .pull_up = true,
        .debounce_time_us = 75000,
        .mqtt_topic = "/pinMonitor/gpio5"};
    debounce_register_pin(&pin5_cfg);
}

/// @brief debounce_entry_t is a structure that represents a debounce entry, containing a GPIO pin (gpio_num_t pin)
/// and an associated timer (esp_timer_handle_t timer). It is used to manage debouncing logic for input
/// signals in embedded systems.
typedef struct
{
    gpio_num_t pin;
    esp_timer_handle_t timer;
} debounce_entry_t;

/// @brief The on_wifi_state_change function is a static callback that handles changes in Wi-Fi state.
/// When the state is WIFI_STATE_CONNECTED, it initializes the MQTT connection and starts monitoring pin
/// states with interrupt service routine (ISR) and debounce logic.
/// @param state
static void on_wifi_state_change(wifi_state_t state)
{
    if (state == WIFI_STATE_CONNECTED)
    {
        mqtt_app_start();   // Your MQTT init
        pin_monitor_init(); // Your ISR + debounce logic
    }
}

/// @brief
/// The wifi_event_handler function is a static event handler that processes Wi-Fi and
/// IP events in an ESP-IDF application. It handles station start and disconnection events
/// by attempting to reconnect and sets a connection status bit when an IP address is
/// obtained.
/// @param arg
/// @param event_base
/// @param event_id
/// @param event_data
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

/// @brief
/// The wifi_init_sta_ext function initializes and configures the Wi-Fi station mode on an
/// ESP32 device. It sets up the necessary event handlers, creates a Wi-Fi configuration
/// using SSID and password stored in NVS (Non-Volatile Storage), starts the Wi-Fi, and waits
/// for a successful connection.
/// @param  none
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
    esp_err_t err = nvs_open("wifi", NVS_READONLY, &nvs_handle);
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

/// @brief Prints the IP information of the specified network interface.
/// @param netif The network interface to query.
void print_ip_info(esp_netif_t *netif)
{
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(netif, &ip_info);

    ESP_LOGI("NETIF", "IP Address: " IPSTR, IP2STR(&ip_info.ip));
    ESP_LOGI("NETIF", "Gateway:   " IPSTR, IP2STR(&ip_info.gw));
    ESP_LOGI("NETIF", "Netmask:   " IPSTR, IP2STR(&ip_info.netmask));
}

/// @brief The mqtt_publish_gpio_event function is a static utility that publishes a message to an MQTT topic
/// indicating that a specified GPIO pin has gone HIGH. It formats the message with the GPIO number,
/// sends it to the /pinMonitor/event topic using the MQTT client, and logs the published message for debugging purposes.
/// @param gpio_num The GPIO number that triggered the event.
/// @return void
static void mqtt_publish_gpio_event(uint32_t gpio_num)
{
    char msg[64];
    snprintf(msg, sizeof(msg), "GPIO %ld went HIGH", gpio_num);
    esp_mqtt_client_publish(mqtt_client, "/pinMonitor/event", msg, 0, 1, 0);
    ESP_LOGI(TAG, "Published: %s", msg);
}

/// @brief GPIO task that monitors GPIO events.
/// @param arg Pointer to task arguments (not used).
static void gpio_task(void *arg)
{
    uint32_t io_num;
    for (;;)
    {
        if (xQueueReceive(gpio_event_queue, &io_num, portMAX_DELAY))
        {
            mqtt_publish_gpio_event(io_num);
        }
    }
}

/// @brief The mqtt_app_start function initializes and starts an MQTT client using the ESP-IDF framework.
/// It configures the client with a broker URI, username, and password, then creates and starts the client instance.
/// @param  none.
/// @return void
void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address.uri = "mqtt://10.0.0.2:1883",
        },
        .credentials = {
            .username = "david1952",
            .authentication.password = "M9JP0Hz2iHaDbSX9CHn5",
        }};
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);

    esp_mqtt_client_start(mqtt_client);
}

/// @brief
/// The app_main function serves as the entry point for an ESP32 application,
/// initializing essential components such as NVS flash, network interfaces, and the event loop.
/// It sets up Wi-Fi, GPIO event handling with debouncing, MQTT communication, and starts
/// tasks for monitoring GPIO pins with specific configurations.
/// @param None
/// @return int
int app_main(void)
{
    static const char *TAG = "PinMonitor";

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Robust Wi-Fi credential check
    char ssid[32] = {0}, pass[64] = {0};
    bool creds_valid = false;
    nvs_handle_t nvs;
    esp_err_t nvs_err = nvs_open("wifi_store", NVS_READONLY, &nvs);
    if (nvs_err == ESP_OK) {
        size_t ssid_len = sizeof(ssid);
        size_t pass_len = sizeof(pass);
        esp_err_t ssid_err = nvs_get_str(nvs, "ssid", ssid, &ssid_len);
        esp_err_t pass_err = nvs_get_str(nvs, "pass", pass, &pass_len);
        nvs_close(nvs);
        if (ssid_err == ESP_OK && pass_err == ESP_OK && ssid[0] != '\0') {
            creds_valid = true;
        }
    }

    if (!creds_valid) {
        ESP_LOGW(TAG, "Wi-Fi credentials not found or invalid in NVS, starting provisioning...");
        ESP_ERROR_CHECK(wifi_provisioning_init());
        ESP_ERROR_CHECK(wifi_provisioning_start());
        ESP_LOGI(TAG, "Skipping normal Wi-Fi and MQTT since credentials are missing or invalid.");
        return 0;
    } else {
        ESP_LOGI(TAG, "Wi-Fi credentials found in NVS, starting normal Wi-Fi...");
        wifi_init_sta_ext(); // Use credentials from NVS
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        print_ip_info(netif);
        mqtt_app_start(); // Now safe to start MQTT
        ESP_LOGI(TAG, "PinMonitor started");
        return 0;
    }
}