#pragma once
#include "debounce.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

TickType_t debounce_ticks;

typedef struct {
    debounce_config_t config;
    TimerHandle_t     tmr;        // FreeRTOS one-shot
} debounce_entry_t;

extern debounce_entry_t debounce_pins[];
extern int debounce_count;
