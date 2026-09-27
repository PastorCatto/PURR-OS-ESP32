#pragma once
// ota_mgr.h — PURR OS OTA update manager (public API)
//
// Wraps esp_https_ota (WiFi delivery) + app_update — fetches a small JSON
// manifest from an NVS-persisted URL, compares its version against
// KITT_VERSION via purr_kernel_version_cmp(), and if newer, downloads and
// writes the new image to the inactive ota_0/ota_1 slot. Requires
// device.pcat [device] ota = true (dual-slot partition table, bootloader
// rollback enabled) — see purrstrap.py's _generate_sdkconfig(). On a device
// built without that flag, ota_mgr_init() declines (PURR_MODULE_INIT_DECLINED)
// rather than running against a partition table with no second slot.
//
// Integrity: after the image is written, SHA-256 of the FLASHED bytes (read
// back via esp_partition_read(), not hashed while streaming — this also
// catches a corrupt WRITE, not just a corrupt transfer) is compared against
// the manifest's declared hash before esp_ota_set_boot_partition() is ever
// called. No secure boot, no signature check — unsigned/self-built firmware
// is an accepted flashing path here by design; this checksum exists to catch
// corruption and mismatched files, not to gate authenticity.
//
// Rollback: purr_crash_guard.c owns marking a freshly-booted OTA image valid
// (or invalid + reverting) — see its own comment on that. ota_mgr's job ends
// at "wrote and verified an image, told the bootloader to boot it next."
//
// SD-card delivery (ota_mgr_apply_from_sd()) is the designed-for, not yet
// built, extension point — see its own doc comment below.

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_MGR_IDLE = 0,
    OTA_MGR_CHECKING,
    OTA_MGR_AVAILABLE,      // ota_mgr_check() found a newer version
    OTA_MGR_UP_TO_DATE,     // ota_mgr_check() found nothing newer
    OTA_MGR_DOWNLOADING,
    OTA_MGR_VERIFYING,
    OTA_MGR_READY_TO_REBOOT,
    OTA_MGR_FAILED,
} ota_mgr_status_t;

int  ota_mgr_init(void);
void ota_mgr_deinit(void);

// True only on a device built with [device] ota = true (a second OTA slot
// actually exists on its partition table). ota_mgr compiles into every
// build that enables settings — same "always REQUIRES'd, runtime-optional"
// shape wifi_mgr/bt_mgr/meshtastic already have there — so Settings' own
// Updates category checks this to show real controls vs. "not available on
// this device" rather than offering buttons that would always harmlessly
// fail on a single-slot build.
bool ota_mgr_is_supported(void);

// Manifest URL — NVS-persisted, same "Settings enters it, module persists
// it" pattern wifi_mgr's ssid/password already uses. No hardcoded default:
// this device does not phone home anywhere the user didn't point it.
void        ota_mgr_set_manifest_url(const char *url);
const char *ota_mgr_manifest_url(void);   // empty string if never set

// Fetches {"version","url","sha256","size"} from the manifest URL and
// compares "version" against KITT_VERSION via purr_kernel_version_cmp().
// Blocking (one small HTTP GET) — call from a worker task, same expectation
// wifi_mgr_scan() already carries for the UI task. Sets status to
// AVAILABLE, UP_TO_DATE, or FAILED (see ota_mgr_error()).
void ota_mgr_check(void);

// Downloads the manifest's image into the inactive OTA slot, verifies its
// SHA-256 against the manifest's declared hash, and on success calls
// esp_ota_set_boot_partition() — the device boots the new image on its next
// reboot; nothing else needs to run. Blocking — call from a worker task.
// Status moves DOWNLOADING -> VERIFYING -> READY_TO_REBOOT, or FAILED at
// any step.
void ota_mgr_apply(void);

// Default SD location Settings' "Install from SD" button points at — matches
// this codebase's existing "/sdcard/apps/", "/sdcard/wallpapers/" convention
// (see app_manager.c). `purrstrap build <device>` (for an OTA-enabled
// device) writes firmware.bin + firmware.bin.sha256 into cattobaked/<device>/
// — copy both, renamed to match these two names, onto the SD card to test
// an update without standing up an HTTP server.
#define OTA_MGR_SD_DEFAULT_DIR   "/sdcard/ota"
#define OTA_MGR_SD_DEFAULT_PATH  "/sdcard/ota/firmware.bin"

// Reads <path> (the app image) and a companion "<path>.sha256" text file
// (bare 64 hex chars, or "sha256sum"-style "<hex>  <filename>" — either
// works, only the first 64 hex characters are read), writes it to the
// inactive OTA slot via esp_ota_begin()/esp_ota_write()/esp_ota_end(), then
// runs the SAME SHA-256-verify-before-commit gate ota_mgr_apply() uses for
// the WiFi path (see that function's doc comment) before calling
// esp_ota_set_boot_partition(). Blocking — call from a worker task, same as
// ota_mgr_check()/ota_mgr_apply(). No version comparison: pointing this at
// a file IS the user's decision to install it, unlike the WiFi path's
// automatic "is this newer" check.
bool ota_mgr_apply_from_sd(const char *path);

ota_mgr_status_t ota_mgr_status(void);
// 0-100. Meaningful only during OTA_MGR_DOWNLOADING/VERIFYING.
int  ota_mgr_progress_percent(void);
// Empty string outside OTA_MGR_FAILED.
const char *ota_mgr_error(void);

// Currently-running firmware version (KITT_VERSION) and the version last
// found by ota_mgr_check() (empty string before the first successful check).
const char *ota_mgr_current_version(void);
const char *ota_mgr_available_version(void);

#ifdef __cplusplus
}
#endif
