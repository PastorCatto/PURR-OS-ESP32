# home — MiniWin's own "startx" app list. Same minimal shape as
# diagnostics/CMakeLists.txt (also uiconf+render_winapi-based) — see
# home.c's own top comment.
idf_component_register(
    SRCS
        home.c
    INCLUDE_DIRS
        .
        ../../../kernel/core
        ../../../kernel/catcalls
        ../../../modules/app_manager
        ../../../modules/uiconf
    REQUIRES esp_common driver freertos app_manager uiconf
)
