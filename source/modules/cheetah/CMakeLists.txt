# cheetah — Windows XP-style desktop backend (favorites icons + a Choose
# Favorites picker). Also the app-widget theme (iOS-7 grouped-table style,
# with dark mode) every app in the system renders through — see cheetah_win.c.
#
# Sources gated on CONFIG_PURR_UI_BACKEND_CHEETAH, same shape as cupcake/mochi/
# tabby's own CMakeLists — devices on another backend don't carry Cheetah's code.
#
# No vendored icon set here: Cheetah reuses mochi's (mochi_icon_for_app() /
# mochi_color_for_app(), see cheetah.h and cheetah_home.c), which is why "mochi" is
# in REQUIRES below and why mochi/CMakeLists.txt's own guard was broadened to
# also compile mochi's sources in when Cheetah is the selected backend — see
# that file's comment for why that's safe (module boot-registration is
# driven by device.pcat's [modules] keys, not by what merely compiled in).
if(CONFIG_PURR_UI_BACKEND_CHEETAH)
    set(CHEETAH_SRCS
        cheetah_module.c
        cheetah_hal.c
        cheetah_home.c
        cheetah_win.c
    )
else()
    set(CHEETAH_SRCS "")
endif()

# app_manager_remote/proximity — cheetah_home.c's launch() now offers to
# download+install a LOCAL/HYBRID-placed remote app on first tap (see
# app_manager_decide_placement(), app_manager.h). Both are conditionally-
# empty on esp32p4/tab5 (no radio there at all) — same "safe to REQUIRE
# unconditionally either way" shape systemui/CMakeLists.txt already
# documents for pairing/app_manager_remote; the C code itself guards the
# actual calls behind CONFIG_IDF_TARGET_ESP32P4 (see cheetah_home.c's own
# #if), matching systemui_login.c's identical precedent.
idf_component_register(
    SRCS ${CHEETAH_SRCS}
    INCLUDE_DIRS
        .
        ../../kernel/core
        ../../kernel/catcalls
        ../systemui
        ../app_manager_remote
        ../proximity
        ../user_mgr
        ../claw_loader
    REQUIRES esp_common driver freertos nvs_flash lvgl__lvgl app_manager systemui mochi app_manager_remote proximity user_mgr claw_loader
)
