# The PURR OS bootloader's `main` component, shared by every project that builds it.
#
# bootloader/ is the one place this source lives. bootloader/ itself, PurrOS/ and Kernel/ each
# have a bootloader_components/main/CMakeLists.txt that sets PURR_BL_MAIN_DIR to this folder
# and include()s this file, so `idf.py flash` from PurrOS/ or Kernel/ flashes the PURR
# bootloader, not IDF's stock one (which used to be built and flashed silently instead).
#
# Paths are absolute: when included from another project, the component's directory is that
# project's bootloader_components/main, not this one.
set(coreos_dir "${PURR_BL_MAIN_DIR}/../../../PurrOS/components/coreos")
idf_component_register(SRCS "${PURR_BL_MAIN_DIR}/purr_boot.c"
                            "${PURR_BL_MAIN_DIR}/purr_bootpkg.c"
                            "${PURR_BL_MAIN_DIR}/purr_bootcfg.c"
                            "${PURR_BL_MAIN_DIR}/purr_handoff.c"
                            "${PURR_BL_MAIN_DIR}/purr_slotverify.c"
                            "${PURR_BL_MAIN_DIR}/purr_crypto_uecc.c"
                            "${coreos_dir}/src/purr_cfg.c"
                            "${coreos_dir}/src/purr_util.c"
                            "${coreos_dir}/src/purr_keybag.c"
                            "${coreos_dir}/src/purr_verify.c"
                            "${coreos_dir}/src/purr_reason.c"
                            "${coreos_dir}/src/purr_trailer.c"
                            "${coreos_dir}/src/purr_trust.c"
                            "${coreos_dir}/keys/purr_default_keys.c"
                       INCLUDE_DIRS "${PURR_BL_MAIN_DIR}" "${coreos_dir}/include"
                       REQUIRES bootloader bootloader_support hal esp_rom soc spi_flash micro-ecc)

# Same linker scripts as the stock bootloader main component
idf_build_get_property(target IDF_TARGET)
set(ld_dir "${IDF_PATH}/components/bootloader/subproject/main/ld/${target}")
target_linker_script(${COMPONENT_LIB} INTERFACE
                     "${ld_dir}/bootloader.ld"
                     "${ld_dir}/bootloader.rom.ld")
