#pragma once

#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief 
/// debounce_config_t is a structure that defines the configuration for a debounced GPIO pin.
/// It includes fields for the pin number (pin), interrupt type (intr_type), pull-up configuration
/// (pull_up), debounce time in microseconds (debounce_time_us), and an optional MQTT topic
/// (mqtt_topic).
typedef struct {
    gpio_num_t pin;
    gpio_int_type_t intr_type;
    bool pull_up;
    uint32_t debounce_time_us;
    const char* mqtt_topic;
} debounce_config_t;

void debounce_init(void);
void debounce_register_pin(const debounce_config_t* config);

#ifdef __cplusplus
}
#endif


/* #pragma once

#include "driver/gpio.h"
#include "esp_timer.h"

#ifdef __cplusplus
extern "C" {
#endif

void debounce_init(void);

typedef struct {
    gpio_num_t pin;
    gpio_int_type_t intr_type;
    bool pull_up;
    uint32_t debounce_time_us;
    const char* mqtt_topic;
} debounce_config_t;

typedef struct {
    debounce_config_t config;
    esp_timer_handle_t timer;
} debounce_entry_t;

#ifdef __cplusplus
}
#endif */