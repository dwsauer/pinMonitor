# components/app_shared/CMakeLists.txt
idf_component_register(
    INCLUDE_DIRS
        "include"
    REQUIRES
        mqtt        # for mqtt_client.h
        freertos    # for FreeRTOS/queues types
)
