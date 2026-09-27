idf_component_register(
    SRCS
        bt_mgr.c
    INCLUDE_DIRS
        .
        ../../kernel/core
        ../../kernel/catcalls
    REQUIRES esp_common driver freertos bt
)
