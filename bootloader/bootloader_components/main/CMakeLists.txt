set(coreos_dir "${CMAKE_CURRENT_LIST_DIR}/../../../PurrOS/components/coreos")
idf_component_register(SRCS "purr_boot.c" "purr_bootpkg.c" "purr_bootcfg.c" "purr_crypto_uecc.c"
                            "${coreos_dir}/src/purr_cfg.c"
                            "${coreos_dir}/src/purr_util.c"
                            "${coreos_dir}/src/purr_keybag.c"
                            "${coreos_dir}/src/purr_verify.c"
                            "${coreos_dir}/keys/purr_default_keys.c"
                       INCLUDE_DIRS "." "${coreos_dir}/include"
                       REQUIRES bootloader bootloader_support hal esp_rom soc spi_flash micro-ecc)

# Same linker scripts as the stock bootloader main component
idf_build_get_property(target IDF_TARGET)
set(ld_dir "${IDF_PATH}/components/bootloader/subproject/main/ld/${target}")
target_linker_script(${COMPONENT_LIB} INTERFACE
                     "${ld_dir}/bootloader.ld"
                     "${ld_dir}/bootloader.rom.ld")
