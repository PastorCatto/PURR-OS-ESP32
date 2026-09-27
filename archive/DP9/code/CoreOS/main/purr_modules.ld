/* purr_modules.ld — linker section for statically-registered PURR OS modules.
 *
 * All PURR_MODULE_REGISTER() definitions land in .purr_modules.
 * The kernel iterates __start_purr_modules → __stop_purr_modules at boot
 * instead of scanning SPIFFS, so no .purr blobs are needed on flash.
 *
 * Placed after .rodata so function pointers inside the structs are valid
 * (they point into IROM, which is mapped before boot.c runs).
 */
SECTIONS {
    .purr_modules :
    {
        __start_purr_modules = .;
        KEEP(*(.purr_modules))
        __stop_purr_modules = .;
    }
}
INSERT AFTER .flash.rodata;
