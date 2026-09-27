/*
 * purr_menu.h - the boot menu's logic (bootloader/SPEC.md section 9).
 *
 * Pure state, no drawing and no input hardware: the boot package feeds it the
 * bootable partitions, the time and the keys, and draws what it says. That keeps
 * it testable on a PC.
 *
 * Rules:
 *  - One entry per bootable partition, in table order. The preferred one (the one
 *    otadata points at) is selected first.
 *  - With something to boot, a countdown runs. If it reaches zero the selected
 *    entry is booted. Any key stops the countdown for good.
 *  - With nothing to boot, the menu waits PURR_MENU_EMPTY_DELAY_MS, then offers
 *    "Internet recovery" as its only entry. It never boots by itself in that case.
 */
#ifndef PURR_MENU_H
#define PURR_MENU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_MENU_MAX_ENTRIES      8
#define PURR_MENU_LABEL_LEN        20
#define PURR_MENU_TIMEOUT_MS       3000u
#define PURR_MENU_EMPTY_DELAY_MS   2000u

typedef enum {
    PURR_MENU_ACT_NONE = 0,       /* nothing chosen yet */
    PURR_MENU_ACT_BOOT,           /* boot entry `index`, a partition from the input list */
    PURR_MENU_ACT_INTERNET_RECOVERY
} purr_menu_action_t;

typedef enum {
    PURR_KEY_NONE = 0,
    PURR_KEY_UP,
    PURR_KEY_DOWN,
    PURR_KEY_ENTER
} purr_key_t;

/*
 * The T-Deck keyboard has no arrow keys, so menus use WASD: w up, s down, enter or d
 * to choose. Only for navigation. A text field (the Wi-Fi password, say) takes the
 * raw character and must not go through this.
 */
purr_key_t purr_key_from_char(char c);

typedef struct {
    char label[PURR_MENU_LABEL_LEN];   /* the partition's name */
    uint8_t bootable;                  /* nonzero if it holds something that can start */
} purr_menu_part_t;

typedef struct {
    purr_menu_action_t action;
    int index;                         /* for ACT_BOOT: index into the input list */
} purr_menu_result_t;

typedef struct {
    struct {
        char label[PURR_MENU_LABEL_LEN];
        int part;                      /* index in the input list, or -1 for recovery */
    } entries[PURR_MENU_MAX_ENTRIES];
    int count;
    int selected;
    uint32_t elapsed_ms;
    uint8_t counting;                  /* a countdown to auto-boot is running */
    uint8_t no_boot_options;           /* the input had nothing bootable */
} purr_menu_t;

/* preferred: index in `parts` to select first, or -1 for the first bootable. */
void purr_menu_init(purr_menu_t *m, const purr_menu_part_t *parts, int nparts, int preferred);

/* Advance by dt_ms and apply one key. Returns the choice once there is one. */
purr_menu_result_t purr_menu_step(purr_menu_t *m, uint32_t dt_ms, purr_key_t key);

/* What to draw: false until the recovery entry has appeared. */
int purr_menu_visible(const purr_menu_t *m);

/* Seconds left on the countdown (rounded up), or -1 if none is running. */
int purr_menu_seconds_left(const purr_menu_t *m);

#ifdef __cplusplus
}
#endif

#endif /* PURR_MENU_H */
