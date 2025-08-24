#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_provisioning_init(void);
esp_err_t wifi_provisioning_start(void);
esp_err_t wifi_provisioning_stop(void);

#ifdef __cplusplus
}
#endif
