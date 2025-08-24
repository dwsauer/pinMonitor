#ifndef MAIN_PRIV_H
#define MAIN_PRIV_H

#include "wifi_manager.h"
#include "esp_event.h"
#include "esp_netif.h"

// Static function declarations moved from main.c

static void on_wifi_state_change(wifi_state_t state);
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static void mqtt_publish_gpio_event(uint32_t gpio_num);
static void gpio_task(void *arg);

#endif // MAIN_PRIV_H