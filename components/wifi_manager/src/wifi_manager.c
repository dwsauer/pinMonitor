#include "wifi_manager.h"
#include "wifi_credentials.h"
//#include "wifi_provisioning.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "wifi_manager";
static wifi_state_t current_state = WIFI_STATE_INIT;
static wifi_manager_callback_t state_cb = NULL;

/**
 * @brief Internal helper to transition state and invoke callback
 */
static void set_state(wifi_state_t new_state) {
    current_state = new_state;
    ESP_LOGI(TAG, "State changed to %d", new_state);
    if (state_cb) state_cb(new_state);
}

wifi_state_t wifi_manager_get_state(void) {
    return current_state;
}

void wifi_manager_start(wifi_manager_callback_t cb) {
    state_cb = cb;
    set_state(WIFI_STATE_INIT);

    // TODO: Initialize NVS and Wi-Fi stack
    // TODO: Check for stored credentials
    // TODO: Attempt connection or enter provisioning mode
}

/**
 * @brief TODO: Implement periodic state machine runner or event-driven transitions
 */
static void wifi_manager_run(void) {
    // TODO: Implement state transitions based on connection status
    // TODO: Handle retries, fallback, and reboot logic
}