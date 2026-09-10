# systemui — status bar, drag-down panels, nav bar/home indicator, Recents,
# lock screen, login. Three visual styles implementing one contract (see
# systemui.h). The login/relock screen used to be one style-independent
# implementation (systemui_login.c, gating on plain CONFIG_PURR_SYSTEMUI) —
# as of 2026-09-01 it's split too: systemui_login_ios.c (a macOS-style login
# window: dimmed wallpaper, avatar, real multi-account row) compiles in
# under CONFIG_PURR_SYSTEMUI_STYLE_IOS, systemui_login.c (the original
# Windows-XP-style welcome screen) now explicitly EXCLUDES that style
# (`!defined(CONFIG_PURR_SYSTEMUI_STYLE_IOS)`) and keeps serving XP/Android
# exactly as before — see each file's own header comment.
#
# Sources are NOT gated here on a CONFIG_ check. Each file carries its own
# #ifdef — CONFIG_PURR_SYSTEMUI for the module as a whole, plus
# CONFIG_PURR_SYSTEMUI_STYLE_ANDROID / _IOS / _XP for which look — and
# compiles to an empty translation unit when it isn't selected. So exactly
# one set of purr_systemui_* symbols is ever defined, while a host backend
# can still call them unconditionally with no #ifdef of its own and the
# linker always resolves (systemui_android.c also supplies the no-op stubs
# when the module is off entirely — same shape meshtastic_module.c uses for
# its own gate).
#
# Listing every style unconditionally, rather than selecting between them
# here, keeps the file set identical across a style switch — so changing
# visual language is a rebuild, not a CMake reconfigure.
#
# meshtastic is in INCLUDE_DIRS/REQUIRES for the lock screen's node-count line,
# which is itself gated behind CONFIG_PURR_FEATURE_MESHTASTIC.
#
# pairing — systemui_login.c's new "Log in to a server" screen (server_login_
# task()) calls pairing_verify_user()/_request_user_access()/_poll_user_
# access()/_register_user_key()/_get_home_base(); not proximity_rpc directly
# (that stays pairing's own concern, reached transitively if this file ever
# needs it — it doesn't today).
#
# app_manager_remote — async_login_success() calls app_manager_remote_
# connect(mac) directly on a successful server login now, instead of
# app_manager_launch_by_name("milkbar"): the hand-off into remote mode is a
# first-class app_manager-level operation (real local-registry close, then
# remote opens), not something routed through launching Milkbar as an
# ordinary local app first. Same conditional-empty-on-esp32p4 shape as
# pairing above (app_manager_remote.c rides proximity_rpc, absent on that
# radio-less target) — safe to REQUIRE unconditionally either way.
idf_component_register(
    SRCS
        systemui_module.c
        systemui_android.c
        systemui_ios.c
        systemui_xp.c
        systemui_login.c
        systemui_login_ios.c
    INCLUDE_DIRS
        .
        ../../kernel/core
        ../../kernel/catcalls
        ../meshtastic
        ../user_mgr
        ../pairing
        ../app_manager_remote
    REQUIRES esp_common driver freertos nvs_flash lvgl__lvgl app_manager app_manager_remote meshtastic user_mgr pairing
)
