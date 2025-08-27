// File: main/main.c
/**
 * @file    main.c
 * @brief   PinMonitor app: Wi-Fi + MQTT + debounced GPIO event publishing.
 *
 * @details
 * Startup flow:
 *  1) Initialize NVS, netif, and event loop (once).
 *  2) If Wi-Fi credentials exist in NVS (namespace @c wifi_store, keys @c ssid, @c password),
 *     connect in STA mode; otherwise start provisioning (SoftAP web UI).
 *  3) Start the MQTT client.
 *  4) Create @c gpio_event_queue, start a task to publish events, and register debounced GPIOs.
 *
 * Debounce:
 *  - The @c debounce component rearms a one-shot timer from the ISR on every GPIO edge.
 *  - When the timer expires, the pin is sampled in task context and an event is queued
 *    **only** if the final stable level equals the configured @c report_level.
 *
 * MQTT:
 *  - This app publishes simple messages for demonstration. You can switch to JSON payloads
 *    or richer schemas easily in @ref gpio_task.
 *
 * Safety notes:
 *  - Avoid duplicating @c esp_netif_init() / @c esp_event_loop_create_default() (do them once).
 *  - Create @c gpio_event_queue before registering pins to avoid a race if an ISR fires early.
 *  - Do not include private headers from components (e.g., @c private/debounce_internal.h).
 */
// File: main/main.c (includes)
#include <stdio.h>                     // snprintf
#include <string.h>                    // strncpy

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"             // xTaskCreate
#include "freertos/queue.h"            // xQueueCreate, xQueueReceive
#include "freertos/event_groups.h"     // EventGroupHandle_t, xEventGroupSetBits

#include "esp_log.h"                   // ESP_LOG*
#include "esp_err.h"                   // ESP_ERROR_CHECK, esp_err_t
#include "nvs_flash.h"                 // nvs_flash_init
#include "nvs.h"                       // nvs_open/get_str/close   <-- add this

#include "esp_event.h"                 // esp_event_loop_create_default, handlers
#include "esp_netif.h"                 // esp_netif_*, IPSTR
#include "esp_wifi.h"                  // Wi-Fi APIs
#include "mqtt_client.h"               // MQTT client APIs

#include "debounce.h"                  // debouncer API (also pulls in driver/gpio.h)
#include "wifi_provisioning.h"         // provisioning init/start
#include "app_shared.h"                // gpio_event_t, extern queue


/* ===== Globals (single definitions here; use extern elsewhere) ===== */
static const char *TAG = "PinMonitor";
QueueHandle_t gpio_event_queue = NULL;
esp_mqtt_client_handle_t mqtt_client = NULL;

/* ===== Forward declarations ===== */
static void gpio_task(void *arg);
static void pin_monitor_init(void);
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);
static void wifi_init_sta_ext(void);
static void print_ip_info(esp_netif_t *netif);
static void mqtt_app_start(void);

/* ===== Wi-Fi sync bits ===== */
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

/* -------------------------------------------------------------------------- */
/*                         GPIO event -> MQTT task                            */
/* -------------------------------------------------------------------------- */

/**
 * @brief Task that consumes @c gpio_event_queue and publishes via MQTT.
 *
 * @note For production, prefer JSON payloads and QoS that matches your reliability needs.
 */
static void gpio_task(void *arg)
{
    gpio_event_t evt;
    for (;;) {
        if (xQueueReceive(gpio_event_queue, &evt, portMAX_DELAY) == pdTRUE) {
            // Example payload (plain text). Swap to JSON if desired.
            char msg[64];
            snprintf(msg, sizeof(msg), "GPIO %d is now %s",
                     (int)evt.pin, evt.level ? "HIGH" : "LOW");

            const char *topic = evt.topic ? evt.topic : "/pinMonitor/event";
            if (mqtt_client) {
                esp_mqtt_client_publish(mqtt_client, topic, msg, 0, 1, 0);
            }
            ESP_LOGI(TAG, "Published: %s  %s", topic, msg);
        }
    }
}

/* -------------------------------------------------------------------------- */
/*                         Debounce + queue setup                             */
/* -------------------------------------------------------------------------- */

/**
 * @brief Create the event queue, start the publisher task, and register debounced GPIOs.
 *
 * @note Prefer @c GPIO_INTR_ANYEDGE + set @c report_level to the final level that should
 *       emit an event. This ensures the opposite transition retriggers the debounce window.
 */
static void pin_monitor_init(void)
{
    ESP_ERROR_CHECK(debounce_init());

    // Ensure the queue exists before any interrupts can fire
    if (!gpio_event_queue) {
        gpio_event_queue = xQueueCreate(10, sizeof(gpio_event_t));
    }

    // Start the dispatcher task
    xTaskCreate(gpio_task, "gpio_task", 4096, NULL, 10, NULL);

    // --- Example GPIO registrations ---
    // NOTE: If your debounce_config_t lacks .pull_down or .report_level (older version),
    //       remove those fields or update the header first.

    debounce_config_t pin4 = {
        .pin = GPIO_NUM_4,
        .intr_type = GPIO_INTR_ANYEDGE,   // prefer ANYEDGE
        .pull_up = true,
        .pull_down = false,
        .debounce_time_us = 50 * 1000,
        .report_level = DEBOUNCE_REPORT_HIGH, // emit only when stable HIGH
        .mqtt_topic = "/pinMonitor/gpio4",
    };
    ESP_ERROR_CHECK(debounce_register_pin(&pin4));

    debounce_config_t pin5 = {
        .pin = GPIO_NUM_5,
        .intr_type = GPIO_INTR_ANYEDGE,
        .pull_up = true,
        .pull_down = false,
        .debounce_time_us = 75 * 1000,
        .report_level = DEBOUNCE_REPORT_LOW,  // emit only when stable LOW
        .mqtt_topic = "/pinMonitor/gpio5",
    };
    ESP_ERROR_CHECK(debounce_register_pin(&pin5));
}

/* -------------------------------------------------------------------------- */
/*                               Wi-Fi helpers                                */
/* -------------------------------------------------------------------------- */

/** @brief Basic Wi-Fi event handler: connect/reconnect & set connected bit. */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/**
 * @brief Initialize Wi-Fi station using credentials from NVS.
 *
 * @details
 * Expects NVS namespace "wifi_store" with keys "ssid" and "password".
 * Assumes @c esp_netif_init and @c esp_event_loop_create_default were already called.
 */
static void wifi_init_sta_ext(void)
{
    wifi_event_group = xEventGroupCreate();

    // These are already done once in app_main():
    // esp_netif_init();
    // esp_event_loop_create_default();

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    // Read SSID and password from NVS
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi_store", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open (wifi_store) failed: %s", esp_err_to_name(err));
        return;
    }

    char ssid[32] = {0};
    char password[64] = {0};
    size_t ssid_len = sizeof(ssid), pass_len = sizeof(password);

    err = nvs_get_str(nvs, "ssid", ssid, &ssid_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SSID not found in NVS: %s", esp_err_to_name(err));
        nvs_close(nvs);
        return;
    }
    err = nvs_get_str(nvs, "password", password, &pass_len);
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Password not found in NVS: %s", esp_err_to_name(err));
        return;
    }

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid));
    strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Wait until connected
    (void)xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);
    ESP_LOGI(TAG, "Connected to Wi-Fi (NVS)");
}

/** @brief Log assigned IP information for a netif. */
static void print_ip_info(esp_netif_t *netif)
{
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        ESP_LOGI("NETIF", "IP Address: " IPSTR, IP2STR(&ip.ip));
        ESP_LOGI("NETIF", "Gateway:   " IPSTR, IP2STR(&ip.gw));
        ESP_LOGI("NETIF", "Netmask:   " IPSTR, IP2STR(&ip.netmask));
    }
}

/* -------------------------------------------------------------------------- */
/*                               MQTT startup                                 */
/* -------------------------------------------------------------------------- */

/**
 * @brief Start the MQTT client.
 *
 * @note Consider reading URI/credentials from NVS (namespace "mqtt_store") instead
 *       of hardcoding secrets if the repo is public.
 */

/* static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = "mqtt://10.0.0.2:1883",
        .credentials.username = "david1952",
        .credentials.authentication.password = "M9JP0Hz2iHaDbSX9CHn5",
    };
    mqtt_client = esp_mqtt_client_init(&cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt_client));
}*/
static void mqtt_app_start(void)
{
    // Read MQTT creds from NVS (written by provisioning page)
    char uri[128]={0}, user[64]={0}, pass[64]={0};
    size_t ulen=sizeof(uri), nlen=sizeof(user), plen=sizeof(pass);
    nvs_handle_t h;
    if (nvs_open("mqtt_store", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_str(h, "uri",  uri,  &ulen);
        nvs_get_str(h, "user", user, &nlen);
        nvs_get_str(h, "pass", pass, &plen);
        nvs_close(h);
    }
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri[0] ? uri : "mqtt://10.0.0.2:1883",
        .credentials.username = user[0] ? user : NULL,
        .credentials.authentication.password = pass[0] ? pass : NULL,
    };
    mqtt_client = esp_mqtt_client_init(&cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt_client));
}
/* -------------------------------------------------------------------------- */
/*                                 app_main                                   */
/* -------------------------------------------------------------------------- */

/**
 * @brief Program entry: init, Wi-Fi/MQTT, and start pin monitoring.
 */
void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

#if CONFIG_PINMON_ERASE_WIFI_ON_BOOT
    ESP_LOGW(TAG, "Wi-Fi credentials forced erase, starting provisioning...");
    ESP_ERROR_CHECK(wifi_provisioning_init());
    ESP_ERROR_CHECK(wifi_provisioning_start());
    ESP_LOGI(TAG, "Skipping normal Wi-Fi and MQTT since credentials are erased.");
    return;
#endif

    // Check Wi-Fi creds before normal flow
    bool creds_valid = false;
    {
        nvs_handle_t nvs;
        if (nvs_open("wifi_store", NVS_READONLY, &nvs) == ESP_OK) {
            char ssid[32] = {0}, pass[64] = {0};
            size_t ssid_len = sizeof(ssid), pass_len = sizeof(pass);
            esp_err_t e1 = nvs_get_str(nvs, "ssid", ssid, &ssid_len);
            esp_err_t e2 = nvs_get_str(nvs, "password", pass, &pass_len);
            nvs_close(nvs);
            creds_valid = (e1 == ESP_OK && e2 == ESP_OK && ssid[0] != '\0');
        }
    }

    if (!creds_valid) {
        ESP_LOGW(TAG, "Wi-Fi credentials missing; starting provisioning...");
        ESP_ERROR_CHECK(wifi_provisioning_init());
        ESP_ERROR_CHECK(wifi_provisioning_start());
        ESP_LOGI(TAG, "Provisioning active; skipping STA/MQTT/monitor.");
        return;
    }

    // Normal STA + MQTT path
    wifi_init_sta_ext();    
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        print_ip_info(netif);
    }
    mqtt_app_start();
    pin_monitor_init();

    ESP_LOGI(TAG, "PinMonitor started");
}
