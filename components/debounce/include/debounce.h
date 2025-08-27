// File: components/debounce/include/debounce.h
/**
 * @file    debounce.h
 * @brief   Public API for GPIO debouncing using FreeRTOS one-shot timers.
 *
 * @details
 * This component provides a lightweight debouncer for input GPIOs. Each pin:
 * - is configured as input with a caller-provided interrupt type (typically ANYEDGE),
 * - (re)arms a one-shot FreeRTOS timer from the ISR on every edge,
 * - is sampled in task context when the timer expires,
 * - emits an event only when the *stable* level equals @ref debounce_config_t::report_level.
 *
 * The debouncer does **not** publish to MQTT directly. Instead, the timer callback
 * pushes a @c gpio_event_t into @c gpio_event_queue for the application to handle
 * (e.g., publish via MQTT in a normal task).
 *
 * @note String pointers in the config (e.g., @c mqtt_topic) must outlive the registration.
 */

#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Final stable level that should trigger an event emission. */
typedef enum {
    DEBOUNCE_REPORT_LOW  = 0,  /**< Emit when the final stable level is LOW.  */
    DEBOUNCE_REPORT_HIGH = 1,  /**< Emit when the final stable level is HIGH. */
} debounce_report_level_t;

/**
 * @brief Configuration for a debounced GPIO pin.
 *
 * Typical wiring patterns:
 * - Pull-up + active-low switch: use @c pull_up=true, @c pull_down=false,
 *   @c intr_type=GPIO_INTR_ANYEDGE, @c report_level=DEBOUNCE_REPORT_LOW.
 * - Pull-down + active-high switch: use @c pull_up=false, @c pull_down=true,
 *   @c intr_type=GPIO_INTR_ANYEDGE, @c report_level=DEBOUNCE_REPORT_HIGH.
 */
typedef struct {
    gpio_num_t       pin;               /**< GPIO number to monitor. */
    gpio_int_type_t  intr_type;         /**< Interrupt type (usually @c GPIO_INTR_ANYEDGE). */
    bool             pull_up;           /**< Enable internal pull-up.  */
    bool             pull_down;         /**< Enable internal pull-down. */
    uint32_t         debounce_time_us;  /**< Debounce window in microseconds. */
    uint8_t          report_level;      /**< 0=LOW, 1=HIGH (see @ref debounce_report_level_t). */
    const char*      mqtt_topic;        /**< Topic pointer (must outlive registration). */
} debounce_config_t;

/**
 * @brief Install the shared GPIO ISR service (idempotent).
 *
 * Safe to call multiple times; returns ESP_ERR_INVALID_STATE if already installed.
 */
esp_err_t debounce_init(void);

/**
 * @brief Register a single GPIO for debouncing.
 *
 * Configures the GPIO, creates a one-shot timer (period clamped to ≥1 tick),
 * and attaches an ISR that rearms the timer on each interrupt for this pin.
 *
 * @param[in] config  Pointer to the pin configuration. Struct is copied by value.
 *                    Pointers inside (e.g., @c mqtt_topic) must remain valid.
 * @retval ESP_OK on success.
 * @retval ESP_ERR_INVALID_ARG if @p config is NULL.
 * @retval ESP_ERR_NO_MEM if out of entries or timer allocation fails.
 * @retval other errors from @c gpio_config or @c gpio_isr_handler_add.
 */
esp_err_t debounce_register_pin(const debounce_config_t* config);

/* ---------- Optional helper (inline) ---------- */
/**
 * @brief Create a typical pull-up, ANYEDGE, 50 ms config.
 *
 * @param pin   GPIO number.
 * @param topic MQTT topic string (must outlive registration).
 * @return      A populated @ref debounce_config_t.
 */
static inline debounce_config_t debounce_make_default(gpio_num_t pin, const char *topic) {
    debounce_config_t cfg;
    cfg.pin = pin;
    cfg.intr_type = GPIO_INTR_ANYEDGE;
    cfg.pull_up = true;
    cfg.pull_down = false;
    cfg.debounce_time_us = 50 * 1000;
    cfg.report_level = DEBOUNCE_REPORT_HIGH;
    cfg.mqtt_topic = topic;
    return cfg;
}

#ifdef __cplusplus
}
#endif
