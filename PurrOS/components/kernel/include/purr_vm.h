/*
 * purr_vm.h - am I running in a virtual machine, and if so, what does it offer?
 *
 * The emulator puts a small device on the I2C bus the keyboard and touch panel share, at address
 * PURR_VM_I2C_ADDR. A real board has nothing there, so the probe is a one-address NACK and the
 * answer is "no". The device describes the virtual hardware the OS cannot learn any other way:
 * today, that the Ethernet controller is on a bus of its own (plan4.md), so the OS opens it
 * instead of sharing the display's bus, which is where the emulated SPI controller loses an
 * interrupt under load.
 *
 * Nothing here is authority. A VM-info device only tells the OS where to find hardware; it never
 * weakens the trust policy, and the security state still comes from the bootloader's handoff.
 */
#pragma once

#include <stdint.h>

#define PURR_VM_I2C_ADDR   0x6e
#define PURR_VM_INFO_LEN   12

/* Byte 4 of the block. */
#define PURR_VM_NIC_OWN_BUS 0x01   /* the Ethernet controller has its own SPI bus (fields below) */
#define PURR_VM_HAS_RADIO   0x02   /* the emulated chip models a Wi-Fi radio. Not set by any emulator today: Espressif's QEMU has none, so the
                                    * PHY never finishes calibrating and esp_wifi_init stalls the boot. Without this bit, the Wi-Fi backend stays off in a VM. */

typedef struct {
    uint8_t present;           /* a VM-info device answered */
    uint8_t version;           /* its layout version (1) */
    uint8_t flags;             /* PURR_VM_* bits */
    uint8_t nic_host;          /* the SPI controller number: 2 = SPI2, 3 = SPI3 (not an IDF host enum) */
    int8_t nic_sclk, nic_mosi, nic_miso, nic_cs;   /* GPIOs; -1 = none */
} purr_vm_info_t;

/* Probes once (the first call), then returns the same answer. Never NULL; present == 0 on a real
 * board. Safe to call after the board's I2C pins are known (purr_kernel_init). */
const purr_vm_info_t *purr_vm_info(void);
