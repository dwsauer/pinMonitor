# File: components/debounce/CMakeLists.txt
idf_component_register(
    SRCS
        "src/debounce.c"
    INCLUDE_DIRS
        "include"           # headers owned by *this* component
    REQUIRES
        app_shared          # <-- gives you app_shared/include on the include path
        freertos
        esp_driver_gpio
        log
)
