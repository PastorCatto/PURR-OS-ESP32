idf_component_register(
    SRCS
        "umac_core.c"
        "musashi/m68kcpu.c"
        "musashi/m68kdasm.c"
        "musashi/m68kops.c"
        "musashi/m68kopac.c"
        "musashi/m68kopdm.c"
        "musashi/m68kopnz.c"
        "umac/umac.c"
        "umac/umac_rom.c"
        "umac/umac_video.c"
    INCLUDE_DIRS "." "musashi" "umac"
    REQUIRES drv_display lib_purr_ipc
)
