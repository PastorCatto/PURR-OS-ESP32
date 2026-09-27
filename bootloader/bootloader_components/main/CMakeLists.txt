idf_component_register(SRCS "purr_boot.c" "purr_bootpkg.c" "purr_bootcfg.c"
                            "${CMAKE_CURRENT_LIST_DIR}/../../../PurrOS/components/coreos/src/purr_cfg.c"
                            "${CMAKE_CURRENT_LIST_DIR}/../../../PurrOS/components/coreos/src/purr_util.c"
                       INCLUDE_DIRS "." "${CMAKE_CURRENT_LIST_DIR}/../../../PurrOS/components/coreos/include"
                       REQUIRES bootloader bootloader_support hal esp_rom soc spi_flash)

# Same linker scripts as the stock bootloader main component
idf_build_get_property(target IDF_TARGET)
set(ld_dir "${IDF_PATH}/components/bootloader/subproject/main/ld/${target}")
target_linker_script(${COMPONENT_LIB} INTERFACE
                     "${ld_dir}/bootloader.ld"
                     "${ld_dir}/bootloader.rom.ld")
