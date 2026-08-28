# purr_os main component — kernel spine
# Selects specialized kernel boot (kernel_<device>/) if present, falls back
# to core/boot.c for all other devices.
# Arduino kernels (kernel_arduino_*) glob .cpp in addition to .c and add arduino REQUIRES.

set(KERNEL_DIR  ${CMAKE_SOURCE_DIR}/../source/kernel/core)
set(CATCALL_DIR ${CMAKE_SOURCE_DIR}/../source/kernel/catcalls)

set(PURR_DEVICE $ENV{PURR_DEVICE})
if(NOT PURR_DEVICE)
    message(FATAL_ERROR "PURR_DEVICE env var not set — run via purrstrap, not idf.py directly")
endif()
set(GLUE_DIR ${CMAKE_SOURCE_DIR}/../cattobaked/${PURR_DEVICE}/glue)

# Guard: the WinCE shell is baked directly into a kernel's boot.c/.cpp (no
# catcall_ui_t wrapper) and only steps aside correctly when MiniWin is the
# selected UI backend (see miniwin_module.c's own CONFIG_PURR_UI_WINCE_SHELL
# check). Enabling WinCE without MiniWin would produce a kernel where no UI
# system ever starts — fail the build instead of shipping a blank screen.
if(CONFIG_PURR_UI_WINCE_SHELL AND NOT CONFIG_PURR_UI_BACKEND_MINIWIN)
    message(FATAL_ERROR "CONFIG_PURR_UI_WINCE_SHELL requires CONFIG_PURR_UI_BACKEND_MINIWIN — check sdkconfig_${PURR_DEVICE} (generated) and sdkconfig_${PURR_DEVICE}.overrides")
endif()

# Check for a specialized kernel folder — kernel_<device>/
set(SPEC_KERNEL_DIR ${CMAKE_SOURCE_DIR}/../source/kernel/kernel_${PURR_DEVICE})
if(EXISTS ${SPEC_KERNEL_DIR})
    file(GLOB SPEC_SRCS ${SPEC_KERNEL_DIR}/*.c ${SPEC_KERNEL_DIR}/*.cpp)
    set(BOOT_SRCS ${SPEC_SRCS})
    message(STATUS "PURR: using specialized kernel: kernel_${PURR_DEVICE}")
    set(SPEC_INCLUDE_DIRS
        ${SPEC_KERNEL_DIR}
        ${CMAKE_SOURCE_DIR}/../source/kernel/kernel_arduino)
else()
    set(BOOT_SRCS ${KERNEL_DIR}/boot.c)
    set(SPEC_INCLUDE_DIRS "")
    message(STATUS "PURR: using generic kernel for ${PURR_DEVICE}")
endif()

# Arduino kernels use Wire for I2C; native kernels use IDF drivers. Which one
# a device is comes from device.pcat's [device] kernel_type field, resolved
# by purrstrap into PURR_KERNEL_TYPE — not guessed from the device name.
set(PURR_KERNEL_TYPE $ENV{PURR_KERNEL_TYPE})
if(NOT PURR_KERNEL_TYPE)
    message(FATAL_ERROR "PURR_KERNEL_TYPE env var not set — run via purrstrap, not idf.py directly")
endif()

if(PURR_KERNEL_TYPE STREQUAL "arduino")
    set(EXTRA_REQUIRES espressif__arduino-esp32)
    message(STATUS "PURR: Arduino kernel — using Wire for I2C peripherals")
else()
    set(EXTRA_REQUIRES "")
endif()

# Drivers this device's kernel links against.
#
# These used to be hardcoded — `st7789 gt911 trackball bbq20 boot_splash` for
# every native device, `st7789 trackball boot_splash` for Arduino — on the
# reasoning that an unreferenced-but-linkable driver is harmless. That held only
# while modulestrap emitted every component into every device's build.
#
# It no longer does: components_manifest.cmake is now generated per device from
# device.pcat, so a hardcoded name here can refer to a component that was never
# added to the build, and CMake fails at configure time rather than harmlessly.
# purrstrap therefore passes PURR_DRIVER_REQUIRES, built from the same
# device.pcat [drivers] section modulestrap selects components from — one source
# of truth for both what is compiled and what is linked.
#
# Read from the ENVIRONMENT, like PURR_KERNEL_TYPE above, and for a reason worth
# stating: ESP-IDF expands component requirements by executing each component's
# CMakeLists.txt in an isolated scope that sees none of the parent's variables,
# cache included. A CMake variable set in CoreOS/CMakeLists.txt is simply
# invisible here — it read as undefined, this took the fallback, and the build
# then required st7789 on a board with an ssd1306. The environment does survive
# into that expansion.
#
# The fallback keeps a hand-run `idf.py` (no purrstrap, so no env var) working
# exactly as before.
set(PURR_DRIVER_REQUIRES_ENV $ENV{PURR_DRIVER_REQUIRES})
if(PURR_DRIVER_REQUIRES_ENV)
    separate_arguments(DRIVER_REQUIRES NATIVE_COMMAND "${PURR_DRIVER_REQUIRES_ENV}")
    message(STATUS "PURR: driver REQUIRES from device.pcat: ${DRIVER_REQUIRES}")
elseif(PURR_KERNEL_TYPE STREQUAL "arduino")
    set(DRIVER_REQUIRES st7789 trackball boot_splash)
else()
    set(DRIVER_REQUIRES st7789 gt911 trackball bbq20 boot_splash)
endif()

# TAB5_REQUIRES removed (2026-07-28), same class as the driver list above.
#
# It hardcoded `m5tab5_bsp` for tab5. Nothing compiled for that device
# references it: the only user is source/kernel/kernel_tab5_m5bsp_legacy/, and
# SPEC_KERNEL_DIR resolves to kernel_${PURR_DEVICE} — kernel_tab5, which does
# not exist — so tab5 builds the generic kernel and the legacy directory is
# never compiled at all. tab5's device.pcat names st7123 as its display.
#
# Harmless while every component was in every build; a configure-time failure
# once components are selected per device. If a Tab5 BSP is wanted again, add it
# to that device.pcat's [drivers] so one file decides.
set(TAB5_REQUIRES "")

# USB MSC (composite CDC+MSC, share the SD card over USB on panic) — pinned/
# dormant (source/modules/usb_msc/DISABLED.md), module.pcat renamed so
# modulestrap no longer discovers it, so the component doesn't exist in the
# build at all right now. Re-add `set(USB_MSC_REQUIRES usb_msc)` for
# tdeck_plus together with un-disabling the module and the #if 0 blocks in
# kernel_tdp_boot.c when this work resumes.
set(USB_MSC_REQUIRES "")

# Per-device boot settle, from device.pcat [device] boot_settle_ms (see
# purr_kernel.c's BOOT_SETTLE_MS). Environment rather than a CMake variable for
# the same reason as PURR_DRIVER_REQUIRES above — component CMakeLists are
# evaluated in an isolated scope. Absent means the kernel's own default.
set(PURR_BOOT_SETTLE_ENV $ENV{PURR_BOOT_SETTLE_MS})

# Whether [drivers].radio names a driver — see purrstrap.py's own comment on
# PURR_HAS_RADIO_DRIVER for why a specialized kernel's boot file needs this
# at compile time rather than just skipping the driver's module registration.
set(PURR_HAS_RADIO_DRIVER_ENV $ENV{PURR_HAS_RADIO_DRIVER})

idf_component_register(
    SRCS
        ${BOOT_SRCS}
        ${KERNEL_DIR}/purr_kernel.c
        ${KERNEL_DIR}/purr_crash_guard.c
        ${GLUE_DIR}/purr_device_glue.c
    INCLUDE_DIRS
        ${KERNEL_DIR}
        ${CATCALL_DIR}
        ${SPEC_INCLUDE_DIRS}
    REQUIRES
        esp_common
        esp_system
        esp_timer
        esp_netif
        esp_event
        esp_wifi
        driver
        freertos
        nvs_flash
        spiffs
        # joltwallet__littlefs — mounts the app_cfg partition (see
        # partitions_16mb_ota.csv), the one genuinely mutable partition in an
        # otherwise semi-immutable image. See mount_app_config_vfs() in this
        # device's own kernel_tdp_boot.c and purr_kernel_app_config_read()/
        # _write() in purr_kernel.h.
        joltwallet__littlefs
        fatfs
        sdmmc
        # esp_app_format — for esp_app_get_description() in purr_crash_guard.c,
        # which keys the strike counter on the running firmware's ELF hash so a
        # reflash starts clean.
        #
        # Explicit, not inherited: tdeck_plus happened to pull this in
        # transitively and built fine, while tdeck has a leaner component set and
        # failed with "esp_app_desc.h: No such file or directory" — a difference
        # only a multi-device bake surfaces.
        esp_app_format
        # app_update — esp_ota_ops.h's esp_ota_get_running_partition()/
        # esp_ota_get_state_partition()/esp_ota_mark_app_valid_cancel_rollback()/
        # esp_ota_mark_app_invalid_rollback_and_reboot(), used by
        # purr_crash_guard.c's OTA rollback block (only active under
        # CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE — see device.pcat [device] ota).
        # Same "explicit, not inherited" reasoning as esp_app_format just above:
        # it happens to already be linkable everywhere via a transitive path
        # from esp_app_desc.h's own esp_app_get_description(), but this makes
        # that dependency real instead of assumed, on every device regardless
        # of whether that transitive path stays true as REQUIRES elsewhere change.
        app_update
        ${DRIVER_REQUIRES}
        ${EXTRA_REQUIRES}
        ${USB_MSC_REQUIRES}
        ${TAB5_REQUIRES}
)

if(PURR_BOOT_SETTLE_ENV)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOOT_SETTLE_MS=${PURR_BOOT_SETTLE_ENV}UL)
    message(STATUS "PURR: boot settle ${PURR_BOOT_SETTLE_ENV} ms (from device.pcat)")
endif()

# Only defined when purrstrap actually set it — a bare `idf.py build` (no
# PURR_HAS_RADIO_DRIVER in the environment at all) leaves kernel_tdp_boot.c's
# own #ifndef fallback (defaults to 1, today's unconditional behaviour) in
# charge, same as PURR_BOOT_SETTLE_ENV's "absent means the kernel default" above.
if(DEFINED ENV{PURR_HAS_RADIO_DRIVER})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PURR_HAS_RADIO_DRIVER=${PURR_HAS_RADIO_DRIVER_ENV})
endif()
