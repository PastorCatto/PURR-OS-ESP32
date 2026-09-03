# app_manager_remote — the new Select Server picker calls app_manager_
# remote_connect() so app_manager's own remote-mode state (what "On this
# server" in the Apps tab actually reads) follows the picked device, not
# just this app's own s_target_mac (used directly for WiFi/push RPCs).
idf_component_register(
    SRCS
        server_manager_app.c
    INCLUDE_DIRS
        .
        ../../../kernel/core
        ../../../kernel/catcalls
        ../../../modules/app_manager
        ../../../modules/app_manager_remote
        ../../../modules/pairing
        ../../../modules/server_mgr
    REQUIRES esp_common driver freertos app_manager app_manager_remote pairing server_mgr
)
