idf_component_register(
    SRCS
        reticulum_app.c
    INCLUDE_DIRS
        .
        ../../../kernel/core
        ../../../kernel/catcalls
        ../../../modules/reticulum
    REQUIRES esp_common driver freertos reticulum
)
