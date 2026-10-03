#include "purr_driver.h"

#include <string.h>

/* ---------------------------------------------------------------- pin registry */

#define PIN_SLOTS 64   /* comfortably above any ESP32/ESP32-S3 GPIO number */

static const char *s_pin_owner[PIN_SLOTS];

static int valid_pin(int pin)
{
    return pin >= 0 && pin < PIN_SLOTS;
}

int purr_pins_claim(int pin, const char *owner)
{
    if (pin == PURR_PIN_NONE) {
        return 0;                 /* nothing to claim */
    }
    if (!valid_pin(pin)) {
        return -1;
    }
    if (s_pin_owner[pin] != NULL && strcmp(s_pin_owner[pin], owner) != 0) {
        return 1;                 /* already claimed by someone else */
    }
    s_pin_owner[pin] = owner;
    return 0;
}

const char *purr_pins_owner(int pin)
{
    return valid_pin(pin) ? s_pin_owner[pin] : NULL;
}

void purr_pins_reset(void)
{
    memset(s_pin_owner, 0, sizeof(s_pin_owner));
}

/* ---------------------------------------------------------------- binding */

#define MAX_TRACKED_DEVICES 16

typedef struct {
    const char *name;
    purr_device_state_t state;
    void *handle;
} device_record_t;

static device_record_t s_records[MAX_TRACKED_DEVICES];
static int s_record_count;

static device_record_t *record_for(const char *name)
{
    for (int i = 0; i < s_record_count; i++) {
        if (strcmp(s_records[i].name, name) == 0) {
            return &s_records[i];
        }
    }
    if (s_record_count < MAX_TRACKED_DEVICES) {
        device_record_t *r = &s_records[s_record_count++];
        r->name = name;
        r->state = PURR_DEV_UNBOUND;
        r->handle = NULL;
        return r;
    }
    return NULL;                  /* table full -- the device is simply never tracked */
}

static const purr_driver_t *find_driver(const char *compatible, const purr_driver_t *const *table,
                                        int table_count)
{
    for (int i = 0; i < table_count; i++) {
        if (strcmp(table[i]->compatible, compatible) == 0) {
            return table[i];
        }
    }
    return NULL;
}

void purr_drivers_bind(const purr_board_t *board, const purr_driver_t *const *table, int table_count)
{
    s_record_count = 0;
    if (board == NULL) {
        return;
    }
    for (int i = 0; i < board->device_count; i++) {
        const purr_device_t *dev = &board->devices[i];
        device_record_t *rec = record_for(dev->name);
        if (rec == NULL) {
            continue;
        }

        int conflict = 0;
        for (int p = 0; p < PURR_MAX_EXTRA_PINS; p++) {
            if (purr_pins_claim(dev->extra_pins[p], dev->name) != 0) {
                conflict = 1;
            }
        }
        if (conflict) {
            rec->state = PURR_DEV_PIN_CONFLICT;
            continue;
        }

        const purr_driver_t *drv = find_driver(dev->compatible, table, table_count);
        if (drv == NULL) {
            rec->state = PURR_DEV_NO_DRIVER;
            continue;
        }

        void *handle = NULL;
        if (drv->probe(dev, board, &handle) != 0) {
            rec->state = PURR_DEV_PROBE_FAILED;
            continue;
        }
        rec->state = PURR_DEV_BOUND;
        rec->handle = handle;
    }
}

/* purr_drivers_bind_all() is NOT defined here on purpose: it calls purr_driver_table(),
 * which only the real kernel build provides (purr_kernel.c) -- a host test supplies its own
 * fake table straight to purr_drivers_bind() instead (see test_driver.c), so this
 * otherwise-ESP-IDF-free file never needs purr_driver_table() to exist to link. */

purr_device_state_t purr_device_state(const char *device_name)
{
    for (int i = 0; i < s_record_count; i++) {
        if (strcmp(s_records[i].name, device_name) == 0) {
            return s_records[i].state;
        }
    }
    return PURR_DEV_UNBOUND;
}

void *purr_device_handle(const char *device_name)
{
    for (int i = 0; i < s_record_count; i++) {
        if (strcmp(s_records[i].name, device_name) == 0) {
            return s_records[i].state == PURR_DEV_BOUND ? s_records[i].handle : NULL;
        }
    }
    return NULL;
}
