# uiconf — backend-agnostic `.pui` UI-config interpreter core. See
# purr_uiconf_core.h for the full picture; catstrap/uiconf.py is the
# build-time compiler this reads the output of.
idf_component_register(
    SRCS
        purr_uiconf_core.c
        purr_uiconf_sources.c
        purr_uiconf_render_fb.c
        purr_uiconf_render_winapi.c
    INCLUDE_DIRS
        .
        ../../kernel/core
        ../../kernel/catcalls
        ../user_mgr
        ../app_manager
        ../pairing
    REQUIRES
        esp_common
        freertos
        user_mgr
        app_manager
        pairing
)
