# MagiDOS — game-mode DOS shell + 8086 emulator.
#
# The CGA renderer still lives under purr_wm_app/ from the pre-0.11 layout. Only
# its output format changed (RGB888 -> RGB565); it never depended on the old
# window manager, so it is pulled in from where it sits rather than moved, to
# keep this change reviewable.
#
# drv_8086 (the emulator) is NOT linked yet — this stage is the host shell only.
# It comes in with the INT 21h shim, when there is something for a loaded
# program to call.
idf_component_register(
    SRCS
        magidos_app.c
        purr_wm_app/magidos/magidos_cga.cpp
    INCLUDE_DIRS
        .
        purr_wm_app/magidos
        ../../../kernel/core
        ../../../kernel/catcalls
        ../../../modules/speed_demon
        # app_manager — for app_manager_notify_exited(), so the launcher stops
        # believing MagiDOS is still running once its task ends. Without it the
        # second launch silently does nothing.
        ../../../modules/app_manager
    REQUIRES esp_common driver freertos speed_demon app_manager
)
