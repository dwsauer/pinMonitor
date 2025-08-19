#pragma once
#include "esp_wifi.h"

esp_err_t wifi_provisioning_init(void);
esp_err_t wifi_provisioning_start(void);
esp_err_t wifi_provisioning_stop(void);