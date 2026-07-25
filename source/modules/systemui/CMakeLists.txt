# systemui — status bar, drag-down panels, nav bar/home indicator, Recents,
# lock screen. Two visual styles implementing one contract (see systemui.h).
#
# Sources are NOT gated here on a CONFIG_ check. Each file carries its own
# #ifdef — CONFIG_PURR_SYSTEMUI for the module as a whole, plus
# CONFIG_PURR_SYSTEMUI_STYLE_ANDROID / _IOS for which look — and compiles to an
# empty translation unit when it isn't selected. So exactly one set of
# purr_systemui_* symbols is ever defined, while a host backend can still call
# them unconditionally with no #ifdef of its own and the linker always resolves
# (systemui_android.c also supplies the no-op stubs when the module is off
# entirely — same shape meshtastic_module.c uses for its own gate).
#
# Listing both styles unconditionally, rather than selecting between them here,
# keeps the file set identical across a style switch — so changing visual
# language is a rebuild, not a CMake reconfigure.
#
# meshtastic is in INCLUDE_DIRS/REQUIRES for the lock screen's node-count line,
# which is itself gated behind CONFIG_PURR_FEATURE_MESHTASTIC.
idf_component_register(
    SRCS
        systemui_module.c
        systemui_android.c
        systemui_ios.c
    INCLUDE_DIRS
        .
        ../../kernel/core
        ../../kernel/catcalls
        ../meshtastic
    REQUIRES esp_common driver freertos nvs_flash lvgl__lvgl app_manager meshtastic
)
