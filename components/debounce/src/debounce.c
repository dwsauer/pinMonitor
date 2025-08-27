// File: components/debounce/debounce.c
/**
 * @file    debounce.c
 * @brief   GPIO debouncing component using FreeRTOS one-shot timers.
 *
 * @details
 * This component debounces GPIO inputs by (re)arming a FreeRTOS one-shot timer
 * from the GPIO ISR. When the timer expires, the pin is sampled in task context.
 * An event is emitted **only** if the stable level equals the configured
 * `report_level` (0=LOW, 1=HIGH). The timer arming uses ISR-safe APIs, keeping
 * the ISR tiny and non-blocking.
 *
 * **Design notes**
 * - The ISR calls `xTimerChangePeriodFromISR()` to restart the one-shot window.
 *   No `esp_timer` calls are used in the ISR.
 * - Tick conversion is clamped to **>= 1 tick** to avoid FreeRTOS assertions
 *   when `CONFIG_FREERTOS_HZ` is low.
 * - The timer’s ID stores a pointer to the per-pin entry so callbacks can reach
 *   configuration/state without globals lookups.
 *
 * **Threading/ISR**
 * - ISR runs in IRAM. Only ISR-safe FreeRTOS calls are used.
 * - Timer callback runs in the Timer Service Task context.
 * - Publishing to MQTT is not done here; we push a `gpio_event_t` to
 *   `gpio_event_queue` for the application to handle in normal task context.
 *
 * **Usage**
 * ```c
 * ESP_ERROR_CHECK(debounce_init());
 * debounce_config_t cfg = {
 *   .pin = GPIO_NUM_4,
 *   .intr_type = GPIO_INTR_ANYEDGE,
 *   .pull_up = true,
 *   .pull_down = false,
 *   .debounce_time_us = 50*1000,
 *   .report_level = 1,                   // only emit when stable HIGH
 *   .mqtt_topic = "/pinMonitor/gpio4",
 * };
 * ESP_ERROR_CHECK(debounce_register_pin(&cfg));
 * ```
 *
 * See: components/debounce/include/debounce.h
 */

#include <stdio.h>                  // snprintf
#include <stdint.h>
#include "esp_err.h"                // esp_err_t, esp_err_to_name
#include "esp_log.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "freertos/queue.h"
#include "debounce.h"               // public API
#include "app_shared.h"             // gpio_event_t, extern QueueHandle_t gpio_event_queue

#ifndef MAX_DEBOUNCE_PINS
  #ifdef CONFIG_PINMON_MAX_DEBOUNCE_PINS
    #define MAX_DEBOUNCE_PINS CONFIG_PINMON_MAX_DEBOUNCE_PINS
  #else
    #define MAX_DEBOUNCE_PINS 10
  #endif
#endif

/// Module log tag.
static const char *TAG = "Debounce";

/** @brief Per-pin internal state (not exposed in public header). */
typedef struct {
    debounce_config_t config;        /**< User configuration for this pin (string pointers must outlive registration). */
    TimerHandle_t     tmr;           /**< One-shot FreeRTOS timer used for the debounce window. */
    TickType_t        debounce_ticks;/**< Cached period in ticks (guaranteed >= 1). */
} debounce_entry_t;

static debounce_entry_t s_entries[MAX_DEBOUNCE_PINS]; /**< Static storage for pin entries. */
static int              s_count = 0;                  /**< Number of registered pins. */

/**
 * @brief FreeRTOS timer callback (task context).
 *
 * Resamples the pin at expiry. If the sampled level equals `report_level`,
 * emits a `gpio_event_t` into `gpio_event_queue`.
 *
 * @param xTimer Timer handle whose ID stores a pointer to the pin entry.
 */
static void debounce_timer_cb(TimerHandle_t xTimer)
{
    debounce_entry_t *e = (debounce_entry_t*) pvTimerGetTimerID(xTimer);
    if (!e) return;

    const int level = gpio_get_level(e->config.pin);

    // Emit only when final stable level equals report_level (0=LOW, 1=HIGH)
    if ((uint8_t)level != (e->config.report_level ? 1u : 0u)) {
        return;
    }

    gpio_event_t evt = {
        .pin   = e->config.pin,
        .level = level,
        .topic = e->config.mqtt_topic, // NOTE: pointer must remain valid
    };

    if (gpio_event_queue) {
        if (xQueueSend(gpio_event_queue, &evt, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Queue full; dropped GPIO %d event", (int)e->config.pin);
        }
    } else {
        ESP_LOGW(TAG, "gpio_event_queue is NULL; event lost (GPIO %d)", (int)e->config.pin);
    }
}

/**
 * @brief GPIO ISR: rearm one-shot debounce timer with cached period.
 *
 * @note Uses only ISR-safe FreeRTOS calls. `xTimerChangePeriodFromISR()` resets
 *       the timer; explicit stop is not required.
 *
 * @param arg Pointer to the `debounce_entry_t` for this pin (installed at register time).
 */
static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    debounce_entry_t *e = (debounce_entry_t*)arg;
    if (!e) return;

    BaseType_t hpw = pdFALSE;
    xTimerChangePeriodFromISR(e->tmr, e->debounce_ticks, &hpw);
    portYIELD_FROM_ISR(hpw);
}

/**
 * @brief Install the shared GPIO ISR service (idempotent).
 *
 * @return ESP_OK on success; ESP_ERR_INVALID_STATE is also accepted (already installed).
 */
esp_err_t debounce_init(void)
{
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

/**
 * @brief Register a GPIO for debouncing.
 *
 * Configures the GPIO as input with the requested interrupt type and pulls,
 * creates a one-shot timer (period cached in ticks and clamped to >=1),
 * then attaches an ISR handler that rearms the timer on any interrupt for this pin.
 *
 * @param config Pointer to the debounce configuration. The struct is copied by value,
 *               but any pointed strings (e.g., `mqtt_topic`) must outlive registration.
 * @retval ESP_OK on success.
 * @retval ESP_ERR_INVALID_ARG if @p config is NULL.
 * @retval ESP_ERR_NO_MEM if out of entries or timer allocation failed.
 * @retval other Propagated error from `gpio_config()` or `gpio_isr_handler_add()`.
 */
esp_err_t debounce_register_pin(const debounce_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    if (s_count >= MAX_DEBOUNCE_PINS) {
        ESP_LOGW(TAG, "Max pins (%d) reached. GPIO %d not added.", MAX_DEBOUNCE_PINS, (int)config->pin);
        return ESP_ERR_NO_MEM;
    }

    // 1) Configure the GPIO
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << config->pin),
        .mode         = GPIO_MODE_INPUT,
        .intr_type    = config->intr_type,          // usually GPIO_INTR_ANYEDGE
        .pull_up_en   = config->pull_up   ? GPIO_PULLUP_ENABLE  : GPIO_PULLUP_DISABLE,
        .pull_down_en = config->pull_down ? GPIO_PULLDOWN_ENABLE: GPIO_PULLDOWN_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed for GPIO %d: %s", (int)config->pin, esp_err_to_name(err));
        return err;
    }

    // 2) Compute & clamp debounce period (≥1 tick) — avoids FreeRTOS 0-tick assert.
    TickType_t ticks = pdMS_TO_TICKS((config->debounce_time_us + 999) / 1000);
    if (ticks == 0) ticks = 1;

    // 3) Create one-shot timer with that period
    char name[16];
    snprintf(name, sizeof(name), "db%u", (unsigned)config->pin);
    TimerHandle_t tmr = xTimerCreate(name, ticks, pdFALSE, NULL, debounce_timer_cb);
    if (!tmr) {
        ESP_LOGE(TAG, "xTimerCreate failed for GPIO %d", (int)config->pin);
        return ESP_ERR_NO_MEM;
    }

    // 4) Fill entry and link timer back to it
    debounce_entry_t *e = &s_entries[s_count];
    e->config = *config;           // NOTE: mqtt_topic pointer is not deep-copied
    e->tmr = tmr;
    e->debounce_ticks = ticks;
    vTimerSetTimerID(tmr, e);

    // 5) Attach ISR for this pin
    err = gpio_isr_handler_add(config->pin, gpio_isr_handler, e);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_handler_add failed for GPIO %d: %s", (int)config->pin, esp_err_to_name(err));
        (void)xTimerDelete(tmr, 0);
        return err;
    }

    s_count++;

    ESP_LOGI(TAG, "Registered: GPIO %d, %sedge, %u us, report=%s, %s%s",
             (int)config->pin,
             (config->intr_type == GPIO_INTR_POSEDGE ? "pos" :
              config->intr_type == GPIO_INTR_NEGEDGE ? "neg" : "any"),
             (unsigned)config->debounce_time_us,
             config->report_level ? "HIGH" : "LOW",
             config->pull_up ? "pull-up" : "",
             config->pull_down ? " pull-down" : "");

    return ESP_OK;
}
