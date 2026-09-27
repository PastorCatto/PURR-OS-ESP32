idf_component_register(SRCS "main.c" "commands.c"
                       REQUIRES kernel coreos littlefs esp_partition esp_system esp_timer heap)
