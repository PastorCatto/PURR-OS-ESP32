idf_component_register(
    SRCS
        adc_battery.c
    INCLUDE_DIRS
        .
        ../../../kernel/core
        ../../../kernel/catcalls
        ../../../modules/purr_quirk
    REQUIRES esp_common driver freertos esp_adc purr_quirk
)
