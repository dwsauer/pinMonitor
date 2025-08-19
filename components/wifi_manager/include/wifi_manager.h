#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Wi-Fi provisioning and connection state
 */
typedef enum {
    WIFI_STATE_INIT,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_FAILED,
    WIFI_STATE_PROVISIONING,
    WIFI_STATE_SAVING_CREDENTIALS,
    WIFI_STATE_REBOOTING
} wifi_state_t;

/**
 * @brief Callback type for state change notifications
 */
typedef void (*wifi_manager_callback_t)(wifi_state_t new_state);

/**
 * @brief Initialize and start the Wi-Fi manager state machine
 *
 * @param cb Optional callback for state changes
 */
void wifi_manager_start(wifi_manager_callback_t cb);

/**
 * @brief Get the current Wi-Fi manager state
 *
 * @return Current state
 */
wifi_state_t wifi_manager_get_state(void);

#ifdef __cplusplus
}
#endif