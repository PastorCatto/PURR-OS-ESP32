idf_component_register(
    SRCS
        milkbar_app.c
    INCLUDE_DIRS
        .
        ../../../kernel/core
        ../../../kernel/catcalls
        ../../../modules/pairing
        ../../../modules/proximity
        ../../../modules/proximity_rpc
        ../../../modules/app_manager_remote
        ../../../modules/meshtastic
        ../../../modules/user_mgr
    # miniwin dropped — confirmed unused (this file only ever calls the
    # portable purr_win_* API; every "cupcake_task" mention in its comments is
    # generic UI-backend-task terminology, not an actual dependency). cupcake
    # itself is ALSO dropped, but it was quietly the one providing lvgl.h's
    # include path (for the LV_SYMBOL_* icon glyphs under CONFIG_PURR_UI_LVGL)
    # transitively — lvgl__lvgl added explicitly below instead of restoring
    # cupcake just to get it back accidentally again.
    #
    # user_mgr — explicit rather than relying on it reaching here
    # transitively through `pairing` (which already REQUIRES it): same
    # "explicit, not inherited" reasoning app_manager's own CMakeLists.txt
    # documents for esp_app_format/app_update. Needed for user_mgr_current_
    # user()/user_mgr_is_admin() — the connection flow's dashboard-vs-
    # desktop routing.
    REQUIRES esp_common driver freertos app_manager pairing proximity proximity_rpc app_manager_remote meshtastic user_mgr lvgl__lvgl
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
