# Merged from drivermgr/hwtest/services/meshdiag/taskmgr's own CMakeLists —
# union of INCLUDE_DIRS/REQUIRES, minus drivermgr's old `miniwin` (the app
# only ever used the portable purr_win_* API, never miniwin directly).
idf_component_register(
    SRCS
        diagnostics.c
    INCLUDE_DIRS
        .
        ../../../kernel/core
        ../../../kernel/catcalls
        ../../../modules/driver_manager
        ../../../modules/bt_mgr
        ../../../modules/app_manager
        ../../../modules/meshtastic
        ../../../lib/lib_mesh_pb
    REQUIRES esp_common driver freertos esp_hw_support app_manager driver_manager bt_mgr meshtastic
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
