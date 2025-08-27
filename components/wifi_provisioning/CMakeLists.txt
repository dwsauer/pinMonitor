# File: components/wifi_provisioning/CMakeLists.txt
idf_component_register(
    SRCS 
        "src/wifi_provisioning.c"
        "src/web_server.c"
    INCLUDE_DIRS 
        "include"
    REQUIRES
        esp_http_server 
        nvs_flash 
        esp_netif 
        esp_event 
        esp_wifi 
        freertos
)
