idf_component_register(
    SRCS
        "src/wifi_credentials.c"
        "src/wifi_manager.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        nvs_flash       # <-- add this
        esp_wifi        # already needed
        esp_netif
        esp_event
        log
)
