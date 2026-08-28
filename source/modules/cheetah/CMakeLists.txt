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

idf_component_register(
    SRCS ${CHEETAH_SRCS}
    INCLUDE_DIRS
        .
        ../../kernel/core
        ../../kernel/catcalls
        ../systemui
    REQUIRES esp_common driver freertos nvs_flash lvgl__lvgl app_manager systemui mochi
)
