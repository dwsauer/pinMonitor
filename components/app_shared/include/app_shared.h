// File: components/app_shared/include/app_shared.h
#pragma once
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

// Event sent from debounce (producer) to the GPIO/MQTT task (consumer)
typedef struct {
    gpio_num_t  pin;          // GPIO number
    int         level;        // 0 or 1
    const char *topic;        // MQTT topic (pointer must stay valid)
} gpio_event_t;

// Global event queue owned by main.c (defined exactly once in main.c)
extern QueueHandle_t gpio_event_queue;

#ifdef __cplusplus
}
#endif
