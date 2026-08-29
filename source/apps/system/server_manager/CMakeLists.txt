idf_component_register(
    SRCS
        server_manager_app.c
    INCLUDE_DIRS
        .
        ../../../kernel/core
        ../../../kernel/catcalls
        ../../../modules/app_manager
        ../../../modules/pairing
        ../../../modules/server_mgr
    REQUIRES esp_common driver freertos app_manager pairing server_mgr
)
