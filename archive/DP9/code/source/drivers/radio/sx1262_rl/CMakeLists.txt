# Vendors RadioLib (source/lib/lib_radiolib) privately into this component,
# same pattern lua_runtime/CMakeLists.txt uses for source/lib/lib_lua —
# modulestrap's component discovery only scans source/drivers/*/*/driver.pcat,
# so a bare source/lib/ folder wouldn't be picked up on its own. Only the
# RadioLib translation units the SX126x/SX1262 call chain actually reaches
# are listed (not the whole 80-file tree) — everything else in RadioLib is
# other radio families/protocols this driver never instantiates.
idf_component_register(
    SRCS
        sx1262_rl.cpp
        ../../../lib/lib_radiolib/Module.cpp
        ../../../lib/lib_radiolib/Hal.cpp
        ../../../lib/lib_radiolib/utils/Utils.cpp
        ../../../lib/lib_radiolib/utils/CRC.cpp
        ../../../lib/lib_radiolib/utils/ConvCode.cpp
        ../../../lib/lib_radiolib/protocols/PhysicalLayer/PhysicalLayer.cpp
        ../../../lib/lib_radiolib/hal/ESP-IDF/EspHal.cpp
        ../../../lib/lib_radiolib/modules/SX126x/SX126x.cpp
        ../../../lib/lib_radiolib/modules/SX126x/SX126x_commands.cpp
        ../../../lib/lib_radiolib/modules/SX126x/SX126x_config.cpp
        ../../../lib/lib_radiolib/modules/SX126x/SX126x_LR_FHSS.cpp
        ../../../lib/lib_radiolib/modules/SX126x/SX1262.cpp
    INCLUDE_DIRS
        .
        ../../../lib/lib_radiolib
        ../../../kernel/core
        ../../../kernel/catcalls
        ../../../modules/purr_quirk
    REQUIRES esp_common driver freertos nvs_flash esp_timer purr_quirk
)

# Suppress warnings from upstream RadioLib sources (not our code to fix).
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
