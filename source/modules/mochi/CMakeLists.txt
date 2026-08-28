# mochi — iOS-style springboard backend.
#
# Sources gated on CONFIG_PURR_UI_BACKEND_MOCHI so devices on another backend
# don't carry Mochi's code — same shape as cupcake/ and tabby/.
#
# The blackpurr icon .c files are vendored by relative path for the same reason
# the other LVGL backends vendor them: modulestrap's component discovery only
# scans source/modules/*/module.pcat and source/drivers/*/*/driver.pcat, so a
# bare source/assets/ folder is never picked up as a component on its own.
#
# Mochi owns the system icon set (see mochi.h), so it compiles in the whole
# 48px Ionicons set rather than the handful the other backends vendor: every
# registered app gets a real icon, and mochi_icon_for_app() is the single
# lookup the rest of the system — including the systemui module's Recents
# cards, via purr_systemui_host_t — goes through.
#
# Listed explicitly rather than globbed: CMake globs are evaluated at configure
# time, so a newly generated icon would not trigger a reconfigure and would
# silently fail to link until a clean build.
#
# OR CONFIG_PURR_UI_BACKEND_CHEETAH: Cheetah (source/modules/cheetah/, née
# Flow) reuses this same icon set and mochi_icon_for_app()/mochi_color_for_app()
# directly rather than keeping a third copy — see cheetah.h's header comment.
# Safe to compile Mochi's sources in as a side effect of Cheetah being
# selected: module boot-registration is driven entirely by device.pcat's
# [modules] keys through purrstrap's generated `extern purr_module_<id>;`
# glue (_generate_glue() in purrstrap.py), not by linker-section auto-
# discovery, so nothing references purr_module_mochi on a device whose
# [modules] ui = "cheetah" — and mochi_init() itself is still independently
# guarded on the (in that case unset) CONFIG_PURR_UI_BACKEND_MOCHI, so it
# no-ops even if somehow called.
if(CONFIG_PURR_UI_BACKEND_MOCHI OR CONFIG_PURR_UI_BACKEND_CHEETAH)
    set(MOCHI_ICON_DIR ../../assets/icons/generated/48)
    set(MOCHI_SRCS
        mochi_module.c
        mochi_hal.c
        mochi_springboard.c
        mochi_win.c
        ${MOCHI_ICON_DIR}/settings.c
        ${MOCHI_ICON_DIR}/about.c
        ${MOCHI_ICON_DIR}/terminal.c
        ${MOCHI_ICON_DIR}/fileman.c
        ${MOCHI_ICON_DIR}/calculator.c
        ${MOCHI_ICON_DIR}/tools.c
        ${MOCHI_ICON_DIR}/hwtest.c
        ${MOCHI_ICON_DIR}/drivermgr.c
        ${MOCHI_ICON_DIR}/milkbar.c
        ${MOCHI_ICON_DIR}/nearby.c
        ${MOCHI_ICON_DIR}/services.c
        ${MOCHI_ICON_DIR}/taskmgr.c
        ${MOCHI_ICON_DIR}/meshdiag.c
        ${MOCHI_ICON_DIR}/msn.c
        ${MOCHI_ICON_DIR}/messages.c
        ${MOCHI_ICON_DIR}/magicmac.c
        ${MOCHI_ICON_DIR}/magidos.c
        ../../assets/wallpapers/generated/wallpaper.c
    )
else()
    set(MOCHI_SRCS "")
endif()

idf_component_register(
    SRCS ${MOCHI_SRCS}
    INCLUDE_DIRS
        .
        ../../kernel/core
        ../../kernel/catcalls
        ../../assets/icons
        ../systemui
    REQUIRES esp_common driver freertos nvs_flash lvgl__lvgl app_manager systemui
)
