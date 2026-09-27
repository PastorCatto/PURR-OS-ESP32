idf_component_register(
    SRCS
        wifi_mgr.c
    INCLUDE_DIRS
        .
        ../../kernel/core
        ../../kernel/catcalls
    REQUIRES esp_common driver freertos nvs_flash esp_wifi esp_event esp_netif
)
