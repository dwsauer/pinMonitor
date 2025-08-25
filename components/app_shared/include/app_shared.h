#ifndef APP_SHARED_H
#define APP_SHARED_H

#ifdef __cplusplus
extern "C" {
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "mqtt_client.h"

// Event passed from debounce.c → main.c via gpio_event_queue
typedef struct {
    gpio_num_t pin;     // GPIO number
    int        level;   // 0 = LOW, 1 = HIGH
    const char *topic;  // MQTT topic for this pin
} gpio_event_t;

// Global handles (DEFINED exactly once in main.c)
extern QueueHandle_t            gpio_event_queue;
extern esp_mqtt_client_handle_t mqtt_client;

#ifdef __cplusplus
}
#endif

#endif // APP_SHARED_H
