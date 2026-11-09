// Verifies the signature trailer of an app slot (bootloader/SPEC.md section 3.1).
#ifndef PURR_SLOTVERIFY_H
#define PURR_SLOTVERIFY_H

#include <stdbool.h>
#include <stdint.h>

#include "bootloader_utility.h"

#include "purr_cfg.h"
#include "purr_verify.h"

// Which app slot a partition is. Decides what it is allowed to hold.
typedef enum {
    PURR_SLOT_KERNEL,     // PURR_IMG_MODULE / PURR_MOD_KERNEL
    PURR_SLOT_KITTENOS,   // PURR_IMG_RECOVERY
    PURR_SLOT_LOADER,     // PURR_IMG_MODULE / PURR_MOD_LOADER
} purr_slot_kind_t;

typedef struct {
    bool                 verified;
    purr_verify_result_t result;     // PURR_V_OK exactly when verified
    uint8_t              key_id;     // the key that signed it; meaningful when verified
} purr_slot_verdict_t;

// Reads the trailer from the last sector of the partition and verifies the image in front of
// it against the key bag (compiled-in defaults plus any overrides in cfg). Uses only the plain
// flash reader, never bootloader_mmap(), so it may run at any point in the boot.
void purr_slot_verify(const esp_partition_pos_t *pos, purr_slot_kind_t kind,
                      const purr_cfg_t *cfg, purr_slot_verdict_t *out);

#endif
