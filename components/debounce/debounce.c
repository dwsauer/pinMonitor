#include "debounce.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "private/debounce_internal.h"
#include "esp_log.h"
#include "mqtt_client.h"

#define DEBOUNCE_TIME_US 50000  // 50 ms
#define MAX_DEBOUNCE_PINS 10
debounce_entry_t debounce_pins[MAX_DEBOUNCE_PINS];
int debounce_count = 0;
//static const char *TAG = "Debounce";
extern esp_mqtt_client_handle_t mqtt_client;

/// @brief The debounce_timer_callback function is a timer callback that processes a
/// GPIO pin's state to determine if it is stable and HIGH. If the pin is stable, it 
/// publishes a message to an MQTT topic and logs the event, using the pin's configuration 
/// from a debounce tracking structure.
void debounce_timer_callback(void* arg) {
    gpio_num_t pin = (gpio_num_t)(intptr_t)arg;
    for (int i = 0; i < debounce_count; i++) {
        if (debounce_pins[i].config.pin == pin) {
            int level = gpio_get_level(pin);
            if (level == 1) {
                char msg[64];
                snprintf(msg, sizeof(msg), "GPIO %d stable LOW", pin);
                esp_mqtt_client_publish(mqtt_client, debounce_pins[i].config.mqtt_topic, msg, 0, 1, 0);
                ESP_LOGI("Debounce", "%s → %s", msg, debounce_pins[i].mqtt_topic);
            }
            break;
        }
    }
}

/// @brief The gpio_isr_handler function is an interrupt service routine (ISR) for handling
/// GPIO interrupts. It identifies the GPIO pin associated with the interrupt, checks it
/// against a list of debounce configurations, and starts a one-shot timer to handle debouncing
/// for the corresponding pin.
/// @param (void *) arg
void gpio_isr_handler(void* arg) {
    gpio_num_t pin = (gpio_num_t)(intptr_t)arg;
    // ESP_LOGI(TAG, "ISR triggered for GPIO %d", pin);
    for (int i = 0; i <  debounce_count; i++) {
        if (debounce_pins[i].config.pin == pin) {
            esp_timer_start_once(debounce_pins[i].timer, debounce_pins[i].config.debounce_time_us);
            break;
        }
    }
}

/// @brief
/// The debounce_register_pin function configures a GPIO pin for input with optional
/// pull-up, sets up an interrupt handler, and initializes a debounce timer for the pin
/// using the provided debounce_config_t configuration. If the maximum number of debounce pins
/// is reached, the function logs a warning and does not add the pin.
///
/// @param config Pointer to the debounce_config_t structure containing
/// the configuration for the pin.
void debounce_register_pin(const debounce_config_t* config) {
    gpio_config_t io_conf = {
        .intr_type = config->intr_type,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << config->pin),
        .pull_up_en = config->pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
    };
    gpio_config(&io_conf);

    char timer_name[16];
    snprintf(timer_name, sizeof(timer_name), "debounce_%d", config->pin);

    esp_timer_create_args_t timer_args = {
        .callback = debounce_timer_callback,
        .arg = (void*)(intptr_t)config->pin,
        .name = timer_name
    };

    esp_timer_handle_t timer;
    esp_timer_create(&timer_args, &timer);

    if (debounce_count < MAX_DEBOUNCE_PINS) {
        debounce_pins[debounce_count++] = (debounce_entry_t){
            .config = *config,
            .timer = timer, 
            .mqtt_topic = config->mqtt_topic,
        };
    } else {
        ESP_LOGW("Debounce", "Max debounce pins reached. Pin %d not added.", config->pin);
    }

    gpio_isr_handler_add(config->pin, gpio_isr_handler, (void*)(intptr_t)config->pin);
}

/// @brief 
/// The debounce_init function initializes the debounce mechanism by installing an 
/// ISR (Interrupt Service Routine) service using the ESP-IDF framework. If the installation 
/// fails with an error other than ESP_ERR_INVALID_STATE, it logs an error message with the
/// failure details.
/// @param  
void debounce_init(void) {
    esp_err_t retval = gpio_install_isr_service(0);
    if (retval != ESP_OK && retval != ESP_ERR_INVALID_STATE) {
        ESP_LOGE("Debounce", "Failed to install ISR service: %s", esp_err_to_name(retval));
    }
}
 