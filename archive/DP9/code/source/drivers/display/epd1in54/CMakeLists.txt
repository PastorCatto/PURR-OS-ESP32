idf_component_register(
    SRCS
        epd1in54.c
    INCLUDE_DIRS
        .
        ../../../kernel/core
        ../../../kernel/catcalls
    REQUIRES esp_common driver freertos
)
