#include "drv_8086.h"
#include "purr_dos_ipc.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

static const char *TAG = "drv_8086";

// ────────────────────────────────────────────────────────────────────────────
// 8086tiny emulator state
// ────────────────────────────────────────────────────────────────────────────

// Include 8086tiny.c to get all its globals and definitions.
// Define this so 8086tiny doesn't try to run main()
#define INCLUDE_8086TINY_NO_MAIN

// Pre-declare mem[] in PSRAM so the 640KB buffer doesn't overflow DRAM.
// MEM_DECLARED_EXTERNALLY tells 8086tiny.c to skip its own declaration.
#include "esp_attr.h"
#define MEM_DECLARED_EXTERNALLY
EXT_RAM_BSS_ATTR unsigned char mem[0x10FFF0];

// Include 8086tiny first so its globals (mem, regs8, regs16, etc.) are available
#include "8086tiny.c"

static drv_8086_frame_cb_t s_frame_cb = NULL;
static bool s_running = false;
static bool s_initialized = false;

// ────────────────────────────────────────────────────────────────────────────
// Hooks called by 8086tiny
// ────────────────────────────────────────────────────────────────────────────

void hook_purr_int(void)
{
    purr_dos_ipc_dispatch(mem);
}

void hook_vram_update(void)
{
    if (s_frame_cb) {
        s_frame_cb(mem + CGA_VRAM_BASE, CGA_COLS, CGA_ROWS);
    }
}

// Initialize the CPU register file pointers.
// bios_table_lookup is filled by drv_8086_load_com/exe after the program image
// is in place (8086tiny reads it from the loaded image in its main loop setup).
static void i8086_init_cpu(void)
{
    if (s_initialized) return;

    regs16 = (unsigned short *)(regs8 = mem + REGS_BASE);
    regs16[REG_CS] = 0xF000;
    regs8[FLAG_TF] = 0;

    s_initialized = true;
    s_running = false;
    ESP_LOGI(TAG, "CPU register file initialised");
}

// Public API
// ────────────────────────────────────────────────────────────────────────────

esp_err_t drv_8086_init(const drv_8086_config_t *cfg)
{
    if (mem == NULL) {
        ESP_LOGE(TAG, "8086tiny memory not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_initialized) {
        return ESP_OK;
    }

    i8086_init_cpu();
    ESP_LOGI(TAG, "init — 640 KB conventional memory");
    return ESP_OK;
}

esp_err_t drv_8086_load_com(const uint8_t *data, size_t len)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "CPU not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // COM: flat binary, loaded at segment:offset 0x0100:0x0100
    // PSP at 0x0100:0x0000
    memset(mem + COM_LOAD_SEG * 16, 0, 256);  // clear PSP
    memcpy(mem + COM_LOAD_SEG * 16 + COM_LOAD_OFF, data, len);

    // Set up registers for COM execution
    regs16[REG_CS] = COM_LOAD_SEG;
    reg_ip = COM_LOAD_OFF;
    regs16[REG_SS] = COM_LOAD_SEG;
    regs16[REG_SP] = 0xFFFE;  // Stack starts at top of segment
    regs16[REG_DS] = COM_LOAD_SEG;
    regs16[REG_ES] = COM_LOAD_SEG;

    s_running = true;
    ESP_LOGI(TAG, "loaded COM %zu bytes at %04X:%04X", len, COM_LOAD_SEG, COM_LOAD_OFF);
    return ESP_OK;
}

// Helper: read little-endian word from buffer
static inline uint16_t read_word_le(const uint8_t *buf, uint32_t offset)
{
    return (uint16_t)buf[offset] | ((uint16_t)buf[offset + 1] << 8);
}

esp_err_t drv_8086_load_exe(const uint8_t *data, size_t len)
{
    // MZ EXE format parser and loader
    // Reads MZ header, calculates load address, applies relocations, sets up registers

    if (!s_initialized) {
        ESP_LOGE(TAG, "CPU not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (len < 0x3C) {
        ESP_LOGE(TAG, "MZ file too small for header");
        return ESP_ERR_INVALID_ARG;
    }

    // Read MZ header fields (little-endian)
    uint16_t last_page_bytes = read_word_le(data, 0x02);   // bytes in last page
    uint16_t pages_total     = read_word_le(data, 0x04);   // total pages
    uint16_t num_relocations = read_word_le(data, 0x06);   // number of relocations
    uint16_t header_para     = read_word_le(data, 0x08);   // header size in paragraphs
    uint16_t min_extra_para  = read_word_le(data, 0x0A);   // min extra memory
    uint16_t max_extra_para  = read_word_le(data, 0x0C);   // max extra memory
    uint16_t initial_ss      = read_word_le(data, 0x0E);   // initial SS segment
    uint16_t initial_sp      = read_word_le(data, 0x10);   // initial SP register
    uint16_t initial_ip      = read_word_le(data, 0x14);   // initial IP register
    uint16_t initial_cs_ofs  = read_word_le(data, 0x16);   // initial CS offset
    uint16_t reloc_table_ofs = read_word_le(data, 0x18);   // relocation table offset

    // Calculate image size
    uint32_t image_size = (pages_total - 1) * 512 + (last_page_bytes ? last_page_bytes : 512);
    uint32_t header_size = header_para * 16;

    if (header_size > len || (header_size + image_size) > len) {
        ESP_LOGE(TAG, "MZ file truncated: header=%" PRIu32 ", image=%" PRIu32 ", actual=%zu",
                 header_size, image_size, len);
        return ESP_ERR_INVALID_ARG;
    }

    // Load address: typically 0x0800 (0800:0000) but can vary
    // For safety, use 0x0800 (matches many DOS extenders)
    uint16_t load_segment = 0x0800;

    // Load image into memory at load_segment:0
    uint32_t load_addr = load_segment * 16;
    memcpy(mem + load_addr, data + header_size, image_size);

    ESP_LOGI(TAG, "MZ: header=%" PRIu32 ", image=%" PRIu32 ", load_seg=%04X, relocs=%u",
             header_size, image_size, load_segment, num_relocations);

    // Apply relocations
    // Each relocation: 4 bytes = offset (2 bytes) + segment (2 bytes)
    for (uint16_t i = 0; i < num_relocations; i++) {
        uint32_t reloc_ofs = reloc_table_ofs + i * 4;
        if (reloc_ofs + 4 > len) {
            ESP_LOGW(TAG, "relocation table truncated at entry %u", i);
            break;
        }

        uint16_t reloc_offset  = read_word_le(data, reloc_ofs);
        uint16_t reloc_segment = read_word_le(data, reloc_ofs + 2);

        // Relocation address in loaded image
        uint32_t reloc_addr = load_addr + (reloc_segment * 16) + reloc_offset;

        // Check bounds
        if (reloc_addr + 2 > load_addr + image_size) {
            ESP_LOGW(TAG, "relocation out of bounds: offset=%u, seg=%u",
                     reloc_offset, reloc_segment);
            continue;
        }

        // Apply relocation: add load_segment to the 16-bit value
        uint16_t *reloc_ptr = (uint16_t *)(mem + reloc_addr);
        uint16_t base_segment = *reloc_ptr;
        *reloc_ptr = base_segment + load_segment;
    }

    // Set up registers for execution
    // CS:IP points to entry point (initial_cs_ofs:initial_ip relative to load)
    regs16[REG_CS] = load_segment + initial_cs_ofs;
    reg_ip = initial_ip;

    // SS:SP points to stack
    regs16[REG_SS] = load_segment + initial_ss;
    regs16[REG_SP] = initial_sp;

    // Initialize other segments
    regs16[REG_DS] = load_segment;
    regs16[REG_ES] = load_segment;

    // Initialize flags to known state
    regs8[FLAG_IF] = 1;  // interrupts enabled
    regs8[FLAG_TF] = 0;  // trap off

    s_running = true;
    ESP_LOGI(TAG, "loaded MZ at %04X, CS:IP=%04X:%04X, SS:SP=%04X:%04X",
             load_segment, regs16[REG_CS], reg_ip, regs16[REG_SS], regs16[REG_SP]);
    return ESP_OK;
}

esp_err_t drv_8086_load_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    fseek(f, 0, SEEK_END);
    size_t len = ftell(f);
    rewind(f);

    uint8_t *buf = malloc(len);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    fread(buf, 1, len, f);
    fclose(f);

    // Detect MZ header
    esp_err_t ret;
    if (len >= 2 && buf[0] == 'M' && buf[1] == 'Z') {
        ret = drv_8086_load_exe(buf, len);
    } else {
        ret = drv_8086_load_com(buf, len);
    }

    free(buf);
    return ret;
}

bool drv_8086_step(void)
{
    if (!s_running || !s_initialized) {
        return false;
    }

    // Execute one instruction cycle
    // 8086tiny's main loop sets opcode_stream = mem + 16*CS + IP
    opcode_stream = mem + 16 * regs16[REG_CS] + reg_ip;

    // Check for exit: CS:IP = 0:0
    if (opcode_stream == mem) {
        s_running = false;
        return false;
    }

    // Execute the instruction at opcode_stream
    // This is extracted from 8086tiny's main loop
    // For now, we'll call the i8086_execute_instruction function
    // which is defined in 8086tiny.c

    // TODO: Wire the actual instruction execution from 8086tiny
    // For now, return that we're running (stub)
    return s_running;
}

void drv_8086_key(uint8_t ascii, uint8_t scancode)
{
    // Write into 8086tiny's keyboard buffer (BIOS data area at offset 0x4A6 in memory)
    // Format: low byte = ASCII/scan code, high byte = flags (Alt, Shift, Ctrl, etc.)
    // After writing, we trigger INT 7 (which 8086tiny polls for keyboard interrupt)

    if (!s_initialized) {
        return;
    }

    // Simple keyboard entry: ASCII in low byte, scancode info in high byte
    // For now, just encode the ASCII character
    uint16_t key_data = (uint16_t)ascii;

    // Store in BIOS data area at 0x4A6
    mem[0x4A6] = (uint8_t)(key_data & 0xFF);
    mem[0x4A7] = (uint8_t)((key_data >> 8) & 0xFF);

    // The keyboard interrupt will be handled by 8086tiny's timer/keyboard polling
    // which checks mem[0x4A6] and generates INT 7 when a key is present

    ESP_LOGD(TAG, "injected key: 0x%02X", ascii);
}

void drv_8086_set_frame_callback(drv_8086_frame_cb_t cb)
{
    s_frame_cb = cb;
}

const uint8_t *drv_8086_vram(void)
{
    return s_initialized ? mem + CGA_VRAM_BASE : NULL;
}
