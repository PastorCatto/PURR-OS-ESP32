#include "purr_menu.h"

/* No libc: this also runs in the boot package, which links none. */

purr_key_t purr_key_from_char(char c)
{
    switch (c) {
    case 'w': case 'W': return PURR_KEY_UP;
    case 's': case 'S': return PURR_KEY_DOWN;
    case 'd': case 'D': case '\r': case '\n': return PURR_KEY_ENTER;
    default: return PURR_KEY_NONE;
    }
}

static void add_entry(purr_menu_t *m, const char *label, int part)
{
    char *dst = m->entries[m->count].label;
    int i = 0;
    for (; i < PURR_MENU_LABEL_LEN - 1 && label[i]; i++) {
        dst[i] = label[i];
    }
    dst[i] = '\0';
    m->entries[m->count].part = part;
    m->count++;
}

void purr_menu_init(purr_menu_t *m, const purr_menu_part_t *parts, int nparts, int preferred)
{
    char *raw = (char *)m;
    for (unsigned i = 0; i < sizeof(*m); i++) {
        raw[i] = 0;
    }
    for (int i = 0; i < nparts && m->count < PURR_MENU_MAX_ENTRIES; i++) {
        if (parts[i].bootable) {
            if (i == preferred) {
                m->selected = m->count;
            }
            add_entry(m, parts[i].label, i);
        }
    }
    if (m->count == 0) {
        m->no_boot_options = 1;
        add_entry(m, "Internet recovery", -1);
    } else {
        m->counting = 1;
    }
}

int purr_menu_visible(const purr_menu_t *m)
{
    return !m->no_boot_options || m->elapsed_ms >= PURR_MENU_EMPTY_DELAY_MS;
}

int purr_menu_seconds_left(const purr_menu_t *m)
{
    if (!m->counting) {
        return -1;
    }
    uint32_t left = m->elapsed_ms >= PURR_MENU_TIMEOUT_MS ? 0 : PURR_MENU_TIMEOUT_MS - m->elapsed_ms;
    return (int)((left + 999) / 1000);
}

static purr_menu_result_t choose(const purr_menu_t *m)
{
    purr_menu_result_t r = {PURR_MENU_ACT_NONE, -1};
    int part = m->entries[m->selected].part;
    if (part < 0) {
        r.action = PURR_MENU_ACT_INTERNET_RECOVERY;
    } else {
        r.action = PURR_MENU_ACT_BOOT;
        r.index = part;
    }
    return r;
}

purr_menu_result_t purr_menu_step(purr_menu_t *m, uint32_t dt_ms, purr_key_t key)
{
    purr_menu_result_t none = {PURR_MENU_ACT_NONE, -1};

    /* Keys do nothing while the recovery entry is still hidden. */
    if (!purr_menu_visible(m)) {
        m->elapsed_ms += dt_ms;
        return none;
    }

    if (key != PURR_KEY_NONE) {
        m->counting = 0;   /* a keypress means someone is here: no auto-boot */
    }
    if (key == PURR_KEY_UP && m->selected > 0) {
        m->selected--;
    } else if (key == PURR_KEY_DOWN && m->selected < m->count - 1) {
        m->selected++;
    } else if (key == PURR_KEY_ENTER) {
        return choose(m);
    }

    m->elapsed_ms += dt_ms;
    if (m->counting && m->elapsed_ms >= PURR_MENU_TIMEOUT_MS) {
        return choose(m);
    }
    return none;
}
