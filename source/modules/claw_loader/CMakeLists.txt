# modules/claw_loader/CMakeLists.txt
#
# user_mgr/app_manager — claw_imports_generated.h (purrstrap.py's
# _generate_claw_imports(), see its own _CLAW_IMPORT_HEADERS comment) now
# scans these two headers alongside purr_kernel.h, so a loaded .claw
# object can call user auth / app-launch functions by name. Both are safe
# to require unconditionally (no esp32p4/Tab5 carve-out, unlike pairing/
# app_manager_remote — see that same comment for why those two are NOT
# here yet).
idf_component_register(
    SRCS
        claw_elf.c
        claw_loader.c
    INCLUDE_DIRS
        .
        ../../kernel/core
        ../../kernel/catcalls
        ../user_mgr
        ../app_manager
    REQUIRES
        esp_common
        freertos
        esp_partition
        user_mgr
        app_manager
)
