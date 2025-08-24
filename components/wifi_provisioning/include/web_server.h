#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the provisioning web server.
 * 
 * @param save_fn Callback that receives (ssid, password) strings from client.
 *                Called in the HTTP server’s task (not ISR safe).
 */
esp_err_t web_server_start(void (*save_fn)(const char *ssid, const char *pass));

/**
 * @brief Stop the web server (if running).
 */
void web_server_stop(void);

#ifdef __cplusplus
}
#endif
