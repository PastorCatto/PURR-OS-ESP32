idf_component_register(
    SRCS
        bbq20.c
    INCLUDE_DIRS
        .
        ../../../kernel/core
        ../../../kernel/catcalls
    REQUIRES esp_common driver freertos nvs_flash
)
