# Vendors Lua 5.4 (source/lib/lib_lua) privately into this component, same
# pattern the meshtastic module already uses for nanopb + the Meshtastic
# protobufs — modulestrap's component discovery only scans
# source/modules/*/module.pcat, so a bare source/lib/ folder wouldn't be
# picked up on its own.
idf_component_register(
    SRCS
        lua_runtime.c
        ../../lib/lib_lua/lapi.c
        ../../lib/lib_lua/lauxlib.c
        ../../lib/lib_lua/lbaselib.c
        ../../lib/lib_lua/lcode.c
        ../../lib/lib_lua/lcorolib.c
        ../../lib/lib_lua/lctype.c
        ../../lib/lib_lua/ldblib.c
        ../../lib/lib_lua/ldebug.c
        ../../lib/lib_lua/ldo.c
        ../../lib/lib_lua/ldump.c
        ../../lib/lib_lua/lfunc.c
        ../../lib/lib_lua/lgc.c
        ../../lib/lib_lua/linit.c
        ../../lib/lib_lua/liolib.c
        ../../lib/lib_lua/llex.c
        ../../lib/lib_lua/lmathlib.c
        ../../lib/lib_lua/lmem.c
        ../../lib/lib_lua/loadlib.c
        ../../lib/lib_lua/lobject.c
        ../../lib/lib_lua/lopcodes.c
        ../../lib/lib_lua/loslib.c
        ../../lib/lib_lua/lparser.c
        ../../lib/lib_lua/lstate.c
        ../../lib/lib_lua/lstring.c
        ../../lib/lib_lua/lstrlib.c
        ../../lib/lib_lua/ltable.c
        ../../lib/lib_lua/ltablib.c
        ../../lib/lib_lua/ltm.c
        ../../lib/lib_lua/lundump.c
        ../../lib/lib_lua/lutf8lib.c
        ../../lib/lib_lua/lvm.c
        ../../lib/lib_lua/lzio.c
    INCLUDE_DIRS
        .
        ../../lib/lib_lua
        ../../kernel/core
        ../../kernel/catcalls
        ../app_manager
        ../meshtastic
    REQUIRES esp_common driver freertos esp_timer spiffs fatfs app_manager meshtastic
)

# ESP32-S3 Xtensa toolchain doesn't expose long long the way Lua 5.4 expects
# by default — LUA_32BITS uses int32 for lua_Integer and float for
# lua_Number, matching PURR-OS-0.11's original vendoring choice for ESP32.
target_compile_definitions(${COMPONENT_LIB} PUBLIC LUA_32BITS=1)

# Suppress warnings from upstream Lua sources (not our code to fix).
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
