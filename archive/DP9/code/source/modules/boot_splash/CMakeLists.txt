idf_component_register(
    SRCS
        boot_splash.c
    INCLUDE_DIRS
        .
        ../../kernel/core
        ../../kernel/catcalls
    REQUIRES esp_common driver freertos
)
