idf_component_register(
    SRCS
        milkbottle_app.c
    INCLUDE_DIRS
        .
        ../../../kernel/core
        ../../../kernel/catcalls
        ../../../modules/pairing
        ../../../modules/proximity
        ../../../modules/proximity_rpc
    REQUIRES esp_common driver freertos miniwin cupcake pairing proximity proximity_rpc
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
