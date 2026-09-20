# modules/claw_loader/CMakeLists.txt
#
# user_mgr/app_manager — claw_imports_generated.h (purrstrap.py's
# _generate_claw_imports(), see its own _CLAW_IMPORT_HEADERS comment) now
# scans these two headers alongside purr_kernel.h, so a loaded .claw
# object can call user auth / app-launch functions by name. Both are safe
# to require unconditionally (no esp32p4/Tab5 carve-out, unlike pairing/
# app_manager_remote — see that same comment for why those two are NOT
# here yet).
#
# lvgl__lvgl — claw_imports_generated.h's own _CLAW_IMPORT_LVGL_ESSENTIALS
# (hand-curated, not header-scanned — LVGL isn't one of this codebase's
# own headers) needs "lvgl.h" for real (uint32_t)&lv_obj_create-style
# address-of expressions to resolve against. LVGL is already statically
# linked into every device's firmware (unused since the UI archive — see
# archive/ui_backends_v1/README.md), so this adds no new footprint, just
# makes its existing addresses reachable by name to a loaded object the
# same way purr_kernel.h's own functions already are.
#
# ../common — claw_imports_generated.h also scans purr_icons.h and
# purr_lv_style.h (real code compiled into main, see each header's own top
# comment), so both have to be includable from here. Headers only: the
# definitions live in main, which claw_loader neither requires nor links.
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
        ../common
    REQUIRES
        esp_common
        freertos
        esp_partition
        user_mgr
        app_manager
        lvgl__lvgl
)
