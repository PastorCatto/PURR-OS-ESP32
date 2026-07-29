# DOOM — PrBoom under Speed Demon.
#
# Three bodies of code live here and they are NOT held to the same standard:
#
#   prboom/     vendored PrBoom 2.5.0, unmodified. Do not tidy it.
#   wad_tables/ vendored precomputed lookup tables (sine/tangent/gamma).
#   compat/     the platform layer. Vendored as a starting point, then largely
#               rewritten for this OS — see each file's header for what changed.
#   doom_app.c  ours, written for PURR OS.
#
# See README.md for provenance and licensing.

# Vendored sources are globbed rather than listed. Normally a listed set is
# better — a glob silently picks up whatever lands in the directory. Here that
# is the point: prboom/ is a third-party drop that gets replaced wholesale if it
# is ever updated, and a hand-maintained list of 70 files would go stale on the
# first re-vendor and fail confusingly.
file(GLOB DOOM_PRBOOM_SRCS ${CMAKE_CURRENT_LIST_DIR}/prboom/*.c)
file(GLOB DOOM_TABLE_SRCS  ${CMAKE_CURRENT_LIST_DIR}/wad_tables/*.c)
file(GLOB DOOM_COMPAT_SRCS ${CMAKE_CURRENT_LIST_DIR}/compat/*.c)

idf_component_register(
    SRCS
        doom_app.c
        ${DOOM_PRBOOM_SRCS}
        ${DOOM_TABLE_SRCS}
        ${DOOM_COMPAT_SRCS}
    INCLUDE_DIRS
        .
        prboom/include
        wad_tables/include
        compat
        ../../../kernel/core
        ../../../kernel/catcalls
        ../../../modules/speed_demon
        ../../../modules/boot_splash
    # app_manager — PRIVATE, for app_manager_notify_exited(): without it the
    # launcher goes on believing DOOM is running and the second launch silently
    # does nothing. Same reasoning as MagiDOS's.
    PRIV_INCLUDE_DIRS ../../../modules/app_manager
    # fatfs/vfs — the WAD is read from /sdcard via stdio. See doom_wad.c.
    # esp_rom  — three vendored files #include "rom/ets_sys.h". Every use of it
    #            is commented out, so the include is vestigial, but the files
    #            are vendored and not to be edited. esp_rom puts the chip-scoped
    #            copy (esp32s3/rom/ets_sys.h) on the include path, which
    #            resolves it without touching third-party source.
    # esp_timer — i_video.c times the synthesised key releases with
    #             esp_timer_get_time().
    REQUIRES esp_common driver freertos speed_demon boot_splash fatfs vfs esp_rom esp_timer
    PRIV_REQUIRES app_manager
)

# PrBoom is 1990s C and this project builds with -Wall -Wextra -Werror=all.
# It will not compile clean and it is not going to be made to: the whole value
# of vendoring it unmodified is that a future re-vendor is a directory copy.
#
# Applied per-source rather than to the whole component deliberately. A
# component-wide target_compile_options() would also silence doom_app.c and the
# rewritten parts of compat/, which are OUR code and must stay under the
# project's normal warning level. This way the vendored files are exempt and
# nothing else is.
#
# -w: inhibit warnings entirely on the vendored files.
#
# Two weaker approaches were tried first and both failed, which is worth
# recording so neither gets attempted again:
#
#   A list of -Wno-error=<name>. Espressif's original component.mk named six;
#   GCC 13 against this 1999 codebase kept producing more (duplicate-decl-
#   specifier, nonnull, address, dangling-else, sizeof-pointer-div,
#   missing-field-initializers, type-limits, ...). Each needed a full rebuild to
#   discover, and because ninja stops at the first failure no single build ever
#   reveals the whole set. It would also be specific to one compiler version.
#
#   Blanket -Wno-error. Does not work here: IDF's own -Werror=all wins, and the
#   warnings came back as errors.
#
# -w wins regardless of flag order because it stops the diagnostic being
# emitted at all, leaving -Werror=all nothing to promote. One flag, no list to
# maintain, immune to toolchain bumps.
#
# The cost is real and accepted: we see no warnings from PrBoom. That is the
# right trade for a third-party drop that is meant to be replaced wholesale
# rather than fixed — and it applies ONLY to prboom/ and wad_tables/. doom_app.c
# and everything in compat/ are ours and stay under the project's full
# -Wall -Wextra -Werror=all, which is what the per-source form buys us over a
# component-wide setting.
set_source_files_properties(
    ${DOOM_PRBOOM_SRCS} ${DOOM_TABLE_SRCS}
    PROPERTIES COMPILE_OPTIONS "-w"
)
