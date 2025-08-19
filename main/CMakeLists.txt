idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES mqtt esp_wifi esp_netif nvs_flash driver debounce wifi_manager wifi_provisioning)