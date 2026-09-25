# esp_tinyusb removed from REQUIRES — pinned/dormant (no device.pcat
# references this module currently), and ESP-IDF evaluates every discovered
# component's REQUIRES during the configure pass regardless of whether it's
# actually linked in, so an unresolvable dependency here broke every
# device's build outright, not just ones that use this module. Matches the
# same revert in CoreOS/main/idf_component.yml and the vendored
# arduino-esp32 CMakeLists.txt patch — re-add all three together when USB
# MSC work actually resumes.
idf_component_register(
    SRCS
        usb_msc.c
    INCLUDE_DIRS
        .
    REQUIRES esp_common driver freertos fatfs sdmmc
)
