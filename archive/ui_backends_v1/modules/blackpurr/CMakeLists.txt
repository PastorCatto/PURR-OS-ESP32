if(CONFIG_PURR_UI_BACKEND_BLACKPURR)
    set(BP_SRCS
        blackpurr_module.c
        blackpurr_shell.c
    )
else()
    set(BP_SRCS "")
endif()

idf_component_register(
    SRCS ${BP_SRCS}
    INCLUDE_DIRS
        .
        ../../kernel/core
        ../../kernel/catcalls
        ../../modules/app_manager
    REQUIRES esp_common driver freertos nvs_flash esp_timer
)
