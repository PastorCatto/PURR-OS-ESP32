set(_ipc_requires "")
if(PURR_ENABLE_LORA)
    list(APPEND _ipc_requires drv_lora)
endif()
if(PURR_ENABLE_WIFI)
    list(APPEND _ipc_requires drv_wifi)
endif()
if(PURR_ENABLE_BT)
    list(APPEND _ipc_requires drv_bt)
endif()

idf_component_register(
    SRCS
        "purr_dos_ipc.c"
    INCLUDE_DIRS "."
    REQUIRES     ${_ipc_requires}
)

# Propagate PURR feature flags — REQUIRES above already handles include dirs
if(PURR_ENABLE_LORA)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PURR_HAS_LORA=1)
endif()
if(PURR_ENABLE_WIFI)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PURR_HAS_WIFI=1)
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../../CoreOS/components/drv_wifi")
endif()
if(PURR_ENABLE_BT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PURR_HAS_BT=1)
endif()
