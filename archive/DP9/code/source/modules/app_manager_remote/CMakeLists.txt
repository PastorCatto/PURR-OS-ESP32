# esp32p4 (tab5): same as proximity/CMakeLists.txt — this rides on
# proximity_rpc, which cannot exist on the radio-less P4.
idf_build_get_property(purr_idf_target IDF_TARGET)
if("${purr_idf_target}" STREQUAL "esp32p4")
    idf_component_register()
    return()
endif()

# claw_loader — app_manager_remote_download()'s destination:
# claw_loader_personal_add() lands a downloaded app in the SAME personal-
# space storage server_mgr.c's own app-push feature already uses. Same
# "works whether or not this device also lists claw_loader in its own
# [modules] section" note server_mgr's CMakeLists.txt already documents —
# claw_loader_init() itself does nothing, no real state to initialize.
#
# proximity — handle_launch()'s own proximity_get_own_caps() (this
# device's HAS_DISPLAY bit) for the REMOTEAPPS_LAUNCH_STATUS_* feedback;
# provider_launch()'s purr_kernel_notify() needs purr_kernel.h (already
# transitively available via ../../kernel/core, added to INCLUDE_DIRS
# explicitly below for clarity).
idf_component_register(
    SRCS
        app_manager_remote.c
    INCLUDE_DIRS
        .
        ../../kernel/core
        ../app_manager
        ../proximity_rpc
        ../proximity
        ../claw_loader
    REQUIRES esp_common driver freertos app_manager proximity_rpc proximity claw_loader
)
