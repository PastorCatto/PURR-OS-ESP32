idf_component_register(
    SRCS
        ota_mgr.c
    INCLUDE_DIRS
        .
        ../../kernel/core
        ../../kernel/catcalls
    REQUIRES
        esp_common
        driver
        freertos
        nvs_flash
        esp_partition
        esp_https_ota
        app_update
        esp_http_client
        mbedtls
        # espressif__cjson — full "vendor__name" form, same convention this
        # tree already uses for other component-manager-fetched deps (see
        # lvgl__lvgl in modules/cupcake, espressif__arduino-esp32 elsewhere).
        # Already present under CoreOS/managed_components/ as a transitive
        # dependency of something else; this is the first first-party
        # component to actually REQUIRE it directly.
        espressif__cjson
)
