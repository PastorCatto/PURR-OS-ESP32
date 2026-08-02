# moy — the portable console (moy core 0.1) as a PURR OS app.
#
# Lua 5.4 is vendored PRIVATELY into this component, the same pattern
# lua_runtime uses (and meshtastic uses for nanopb). That is not duplication for
# its own sake — it is what makes the app work under speed demon.
#
# Speed demon unloads the lua_runtime MODULE (PURR_MOD_SYSTEM), so a cart that
# borrowed that interpreter would have it deleted underneath it the moment the
# app started. Linking our own copy means nothing can take it away, and the
# console gets the whole panel. It is also the correct design on the spec's own
# terms: moy requires a sandboxed VM with per-cart state and its own tick, which
# is not what a shared system-wide runtime provides.
#
# The two components never coexist in one image's Lua state — each has its own
# lua_State — and the linker keeps the symbols apart because both are static
# within their component.

set(LUA_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../lib/lib_lua)

# Listed rather than globbed, unlike DOOM's vendored PrBoom. Two of the files in
# lib_lua are deliberately EXCLUDED — liolib.c, loslib.c and loadlib.c are the
# io/os/package libraries, which spec 4.1 forbids a cart from having. Leaving
# them out of the build means they cannot be reached even by a bug in the
# sandbox setup, which a glob would quietly undo the next time someone re-vendors
# Lua.
set(LUA_SRCS
    ${LUA_DIR}/lapi.c     ${LUA_DIR}/lauxlib.c  ${LUA_DIR}/lbaselib.c
    ${LUA_DIR}/lcode.c    ${LUA_DIR}/lcorolib.c ${LUA_DIR}/lctype.c
    ${LUA_DIR}/ldebug.c   ${LUA_DIR}/ldo.c      ${LUA_DIR}/ldump.c
    ${LUA_DIR}/lfunc.c    ${LUA_DIR}/lgc.c      ${LUA_DIR}/llex.c
    ${LUA_DIR}/lmathlib.c ${LUA_DIR}/lmem.c     ${LUA_DIR}/lobject.c
    ${LUA_DIR}/lopcodes.c ${LUA_DIR}/lparser.c  ${LUA_DIR}/lstate.c
    ${LUA_DIR}/lstring.c  ${LUA_DIR}/lstrlib.c  ${LUA_DIR}/ltable.c
    ${LUA_DIR}/ltablib.c  ${LUA_DIR}/ltm.c      ${LUA_DIR}/lundump.c
    ${LUA_DIR}/lutf8lib.c ${LUA_DIR}/lvm.c      ${LUA_DIR}/lzio.c
)

idf_component_register(
    SRCS
        moy_app.c
        moy_cart.c
        moy_draw.c
        moy_input.c
        moy_menu.c
        moy_lua.c
        moy_data.c
        ${LUA_SRCS}
    INCLUDE_DIRS
        .
        ../../../lib/lib_lua
        ../../../kernel/core
        ../../../kernel/catcalls
        ../../../modules/speed_demon
        ../../../modules/boot_splash
    PRIV_INCLUDE_DIRS ../../../modules/app_manager
    # espressif__cjson — manifest.json and config.json (spec 3.1, 9).
    # nvs_flash       — pmem() persistence (spec 9).
    REQUIRES esp_common driver freertos esp_timer speed_demon boot_splash
             fatfs vfs nvs_flash espressif__cjson
    PRIV_REQUIRES app_manager
)

# Lua is third-party and builds with warnings under this project's
# -Wall -Wextra -Werror=all. Same treatment as DOOM's PrBoom and for the same
# reason: it is a vendored drop we do not modify. Applied per-source so that our
# own moy_*.c stay under full warnings — see docs/16_PortingApps.md 3.
set_source_files_properties(${LUA_SRCS} PROPERTIES COMPILE_OPTIONS "-w")
