#pragma once

#include "debounce.h"           // Public config struct
#include "esp_timer.h"          // For esp_timer_handle_t
#include "driver/gpio.h"        // For gpio_num_t, gpio_int_type_t
#include "esp_intr_alloc.h"     // For IRAM_ATTR
#include "esp_attr.h"           // Defines IRAM_ATTR

typedef struct {
    debounce_config_t config;
    esp_timer_handle_t timer;
    const char* mqtt_topic;     // MQTT topic for publishing messages
} debounce_entry_t;

extern debounce_entry_t debounce_pins[];
extern int debounce_count;

void IRAM_ATTR gpio_isr_handler(void* arg);
void debounce_timer_callback(void* arg);
