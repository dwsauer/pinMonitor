#include "debounce.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "private/debounce_internal.h"
#include "esp_log.h"
#include "app_shared.h"   // for gpio_event_t and gpio_event_queue

#define MAX_DEBOUNCE_PINS 10

static const char *TAG = "Debounce";

debounce_entry_t debounce_pins[MAX_DEBOUNCE_PINS];
int debounce_count = 0;

/**
 * Timer callback (NOT ISR). Reads the stable pin level and pushes a gpio_event_t
 * to gpio_event_queue so main.c can publish over MQTT.
 */
static void debounce_timer_callback(void *arg) {
    gpio_num_t pin = (gpio_num_t)(intptr_t)arg;

    for (int i = 0; i < debounce_count; i++) {
        if (debounce_pins[i].config.pin == pin) {
            int level = gpio_get_level(pin);

            gpio_event_t evt = {
                .pin   = pin,
                .level = level,
                .topic = debounce_pins[i].config.mqtt_topic,
            };

            if (gpio_event_queue) {
                BaseType_t ok = xQueueSend(gpio_event_queue, &evt, 0); // non-blocking
                if (ok != pdTRUE) {
                    ESP_LOGW(TAG, "Queue full; dropped GPIO %d event", pin);
                }
            } else {
                ESP_LOGW(TAG, "gpio_event_queue is NULL; event lost (GPIO %d)", pin);
            }
            break;
        }
    }
}

/**
 * GPIO ISR: keep it tiny. Just arm the per-pin debounce one-shot timer.
 */
static void gpio_isr_handler(void *arg) {
    gpio_num_t pin = (gpio_num_t)(intptr_t)arg;

    for (int i = 0; i < debounce_count; i++) {
        if (debounce_pins[i].config.pin == pin) {
            // Stop any pending one-shot so rapid edges don't queue multiple callbacks
            (void)esp_timer_stop(debounce_pins[i].timer);
            (void)esp_timer_start_once(debounce_pins[i].timer,
                                       debounce_pins[i].config.debounce_time_us);
            break;
        }
    }
}

/**
 * Register a pin for debouncing: configures GPIO, creates a one-shot timer,
 * and attaches the ISR handler.
 */
void debounce_register_pin(const debounce_config_t *config) {
    if (debounce_count >= MAX_DEBOUNCE_PINS) {
        ESP_LOGW(TAG, "Max debounce pins reached. Pin %d not added.", config->pin);
        return;
    }

    gpio_config_t io_conf = {
        .intr_type = config->intr_type,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << config->pin),
        .pull_up_en = config->pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed for GPIO %d: %s", config->pin, esp_err_to_name(err));
        return;
    }

    char timer_name[16];
    snprintf(timer_name, sizeof(timer_name), "debounce_%d", config->pin);

    esp_timer_create_args_t timer_args = {
        .callback = debounce_timer_callback,
        .arg = (void *)(intptr_t)config->pin,
        .name = timer_name,
        .dispatch_method = ESP_TIMER_TASK
    };

    esp_timer_handle_t timer = NULL;
    err = esp_timer_create(&timer_args, &timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create failed for GPIO %d: %s", config->pin, esp_err_to_name(err));
        return;
    }

    debounce_pins[debounce_count++] = (debounce_entry_t){
        .config = *config,
        .timer  = timer,
        .mqtt_topic = config->mqtt_topic, // if your internal struct mirrors this member
    };

    err = gpio_isr_handler_add(config->pin, gpio_isr_handler, (void *)(intptr_t)config->pin);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_handler_add failed for GPIO %d: %s", config->pin, esp_err_to_name(err));
        // Clean up timer on failure to attach ISR
        (void)esp_timer_delete(timer);
        debounce_count--;
        return;
    }
    ESP_LOGI(TAG, "Debounce registered: GPIO %d, %sedge, %uus",
             config->pin,
             (config->intr_type == GPIO_INTR_POSEDGE ? "pos" :
              config->intr_type == GPIO_INTR_NEGEDGE ? "neg" : "any "),
             (unsigned)config->debounce_time_us);
}

/**
 * Install the ISR service once. It's OK if it's already installed.
 */
void debounce_init(void) {
    esp_err_t retval = gpio_install_isr_service(0);
    if (retval != ESP_OK && retval != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to install ISR service: %s", esp_err_to_name(retval));
    }
}
