usb_msc pinned/dormant this session — module.pcat renamed to module.pcat.disabled
so modulestrap's directory scan (which looks for a file literally named
module.pcat) skips it entirely. Without this, ESP-IDF compiles every
discovered component into its own static library before link-time pruning,
so usb_msc.c's unconditional #include "tinyusb.h" broke every device's
build outright once the esp_tinyusb dependency was removed (CoreOS/main/
idf_component.yml) — not just devices that actually use it.
Rename back to module.pcat and re-run modulestrap when USB MSC work resumes
(also re-add esp_tinyusb to idf_component.yml, usb_msc's own CMakeLists.txt
REQUIRES, and the vendored arduino-esp32 priv_requires patch).
