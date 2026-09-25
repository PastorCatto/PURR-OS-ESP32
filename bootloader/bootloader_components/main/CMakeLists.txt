idf_component_register(SRCS "purr_boot.c"
                       REQUIRES bootloader bootloader_support)

# Same linker scripts as the stock bootloader main component
idf_build_get_property(target IDF_TARGET)
set(ld_dir "${IDF_PATH}/components/bootloader/subproject/main/ld/${target}")
target_linker_script(${COMPONENT_LIB} INTERFACE
                     "${ld_dir}/bootloader.ld"
                     "${ld_dir}/bootloader.rom.ld")
