idf_component_register(
    SRCS
        milkbar_app.c
    INCLUDE_DIRS
        .
        ../../../kernel/core
        ../../../kernel/catcalls
        ../../../modules/pairing
        ../../../modules/proximity
        ../../../modules/app_manager_remote
        ../../../modules/systemui
        ../../../modules/user_mgr
    # miniwin dropped — confirmed unused (this file only ever calls the
    # portable purr_win_* API; every "cupcake_task" mention in its comments is
    # generic UI-backend-task terminology, not an actual dependency). cupcake
    # itself is ALSO dropped, same reason.
    #
    # proximity_rpc/meshtastic/lvgl__lvgl dropped — this file no longer makes
    # a proximity_rpc_call() of its own (the old bespoke remote app list —
    # Launch/Stop/Milk Bottle/Send — is gone; app_manager_remote.c owns that
    # transport now, reached only through app_manager_remote_connect()/
    # _disconnect(), see this file's top comment) and never used raw LVGL for
    # anything but that same removed screen's icons.
    #
    # app_manager, systemui — explicit rather than relying on either reaching
    # here transitively (app_manager already via `pairing`? no — pairing
    # does NOT require app_manager, so this was never transitive; systemui
    # DOES require app_manager, but same "explicit, not inherited" reasoning
    # app_manager's own CMakeLists.txt documents for esp_app_format/
    # app_update applies here too). app_manager for app_manager_is_remote()/
    # user_mgr_current_user()-adjacent dashboard-vs-desktop routing; systemui
    # for purr_systemui_return_home() (enter_remote_desktop()'s hand-off to
    # the real launcher UI).
    #
    # user_mgr — explicit rather than relying on it reaching here
    # transitively through `pairing` (which already REQUIRES it): same
    # "explicit, not inherited" reasoning as above. Needed for
    # user_mgr_current_user()/user_mgr_is_admin()/user_mgr_is_logged_in()/
    # user_mgr_account_type() — the post-login dashboard-vs-desktop routing.
    REQUIRES esp_common driver freertos app_manager pairing proximity app_manager_remote systemui user_mgr
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
