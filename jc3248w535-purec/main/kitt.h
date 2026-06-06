// KITT — PURR OS kernel boot/runtime, pure-C port for JC3248W535.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_OS_VERSION "0.7.0"
#define KITT_VERSION    "0.4.0"

bool kitt_init(const char* device_json_path);
void kitt_update(void);

bool kitt_is_ready(void);
const char* kitt_device_name(void);
const char* kitt_os_name(void);

// Display text API (size-2 line rows)
void kitt_text_print(uint8_t row, const char* text);
void kitt_text_clear(void);
void kitt_text_set_color(uint32_t fg_hex, uint32_t bg_hex);
void kitt_show_boot_splash(void);
void kitt_emergency_text(const char* l1, const char* l2, const char* l3);
void kitt_log(const char* tag, const char* message);

#ifdef __cplusplus
}
#endif
