idf_component_register(
    SRCS
        "magidos_app.cpp"
        "magidos_filepicker.cpp"
        "magidos_cga.cpp"
    INCLUDE_DIRS "."
    REQUIRES drv_8086 lib_purr_dos_ipc purr_wm drv_display
)
