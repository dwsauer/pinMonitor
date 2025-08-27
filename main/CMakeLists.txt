# File: main/CMakeLists.txt
idf_component_register(
    SRCS 
        "main.c"
    INCLUDE_DIRS 
        "."
    REQUIRES 
        app_shared 
        debounce 
        wifi_provisioning 
        esp_wifi 
        esp_netif 
        nvs_flash 
        mqtt
)
