#ifndef DEBOUNCE_INTERNAL_H
#define DEBOUNCE_INTERNAL_H

#include "debounce.h"     // debounce_config_t
#include "esp_timer.h"    // esp_timer_handle_t
#include "driver/gpio.h"  // gpio_num_t

// Internal tracking for each debounced pin.
typedef struct {
    debounce_config_t   config;      // Public-facing pin config (includes mqtt_topic)
    esp_timer_handle_t  timer;       // One-shot debounce timer
    const char         *mqtt_topic;  // Cached pointer to config.mqtt_topic (optional convenience)
} debounce_entry_t;

// Storage defined in debounce.c
extern debounce_entry_t debounce_pins[];
extern int              debounce_count;

// NOTE:
// - ISR and timer callback are intentionally NOT declared here.
//   They are file-local (static) in debounce.c, so no external prototypes are exposed.

#endif // DEBOUNCE_INTERNAL_H
