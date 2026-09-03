# modules/claw_loader/CMakeLists.txt
idf_component_register(
    SRCS
        claw_elf.c
        claw_loader.c
    INCLUDE_DIRS
        .
        ../../kernel/core
        ../../kernel/catcalls
    REQUIRES
        esp_common
        freertos
        esp_partition
)
