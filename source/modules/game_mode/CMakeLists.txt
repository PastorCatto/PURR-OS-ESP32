idf_component_register(
    SRCS
        game_mode.c
    INCLUDE_DIRS
        .
        ../../kernel/core
        ../../kernel/catcalls
        ../boot_splash
    REQUIRES esp_common driver freertos boot_splash
)
