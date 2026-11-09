idf_component_register(SRCS "kernel_main.c" "kernel_table.c" "loader.c"
                       REQUIRES kernel coreos esp_mm esp_timer heap)
