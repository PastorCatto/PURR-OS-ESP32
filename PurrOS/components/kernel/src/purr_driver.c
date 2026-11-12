#include "purr_driver.h"
#include "purr_driver_class.h"

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

void purr_pins_release_owner(const char *owner)
{
    for (int i = 0; i < PIN_SLOTS; i++) {
        if (s_pin_owner[i] != NULL && strcmp(s_pin_owner[i], owner) == 0) {
            s_pin_owner[i] = NULL;
        }
    }
}

/* ---------------------------------------------------------------- binding */

#define MAX_TRACKED_DEVICES 24

typedef struct {
    const char *name;
    purr_device_state_t state;
    void *handle;
    const purr_driver_t *driver;
} device_record_t;

static device_record_t s_records[MAX_TRACKED_DEVICES];
static int s_record_count;

/* Devices the kernel declares at run time (purr_device_add): a chip the board table cannot list because only the machine it is running on knows it is there (the
 * Ethernet controller of a virtual device). They bind exactly like the board's own devices, and a module registered later finds them too. */
#define MAX_EXTRA_DEVICES 4
static const purr_device_t *s_extra[MAX_EXTRA_DEVICES];
static int s_extra_count;

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
        r->driver = NULL;
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

/* Shared by purr_drivers_bind() (the cold-boot pass) and purr_driver_register() (the
 * plug-and-play one): claim this device's pins, find a driver, probe it, record the outcome.
 * Neither caller touches `rec` before this, so there is exactly one place this logic can
 * drift between the two call sites. */
static int s_net_settled;
static const purr_driver_t *const *s_boot_table;
static int s_boot_table_count;

__attribute__((weak)) void purr_driver_guard(const char *driver_name, int begin)
{
    (void)driver_name;
    (void)begin;
}

static void bind_device(device_record_t *rec, const purr_device_t *dev, const purr_board_t *board,
                        const purr_driver_t *const *table, int table_count)
{
    if (dev->defer == PURR_DEFER_NET && !s_net_settled) {
        rec->state = PURR_DEV_DEFERRED;             /* before any pin is claimed or anything is probed: Wi-Fi is starting, and this waits its turn */
        return;
    }
    int conflict = 0;
    for (int p = 0; p < PURR_MAX_EXTRA_PINS; p++) {
        if (purr_pins_claim(dev->extra_pins[p], dev->name) != 0) {
            conflict = 1;
        }
    }
    if (conflict) {
        rec->state = PURR_DEV_PIN_CONFLICT;
        return;
    }

    const purr_driver_t *drv = find_driver(dev->compatible, table, table_count);
    if (drv == NULL) {
        rec->state = PURR_DEV_NO_DRIVER;
        return;
    }

    void *handle = NULL;
    purr_driver_guard(drv->name, 1);
    int probed = drv->probe(dev, board, &handle);
    purr_driver_guard(drv->name, 0);
    if (probed != 0) {
        rec->state = PURR_DEV_PROBE_FAILED;
        return;
    }
    rec->state = PURR_DEV_BOUND;
    rec->handle = handle;
    rec->driver = drv;
}

void purr_drivers_bind(const purr_board_t *board, const purr_driver_t *const *table, int table_count)
{
    s_boot_table = table;
    s_boot_table_count = table_count;
    s_record_count = 0;
    s_extra_count = 0;
    if (board == NULL) {
        return;
    }
    for (int i = 0; i < board->device_count; i++) {
        const purr_device_t *dev = &board->devices[i];
        device_record_t *rec = record_for(dev->name);
        if (rec == NULL) {
            continue;
        }
        bind_device(rec, dev, board, table, table_count);
    }
}

/* purr_drivers_bind_all() is NOT defined here on purpose: it calls purr_driver_table(),
 * which only the real kernel build provides (purr_kernel.c) -- a host test supplies its own
 * fake table straight to purr_drivers_bind() instead (see test_driver.c), so this
 * otherwise-ESP-IDF-free file never needs purr_driver_table() to exist to link. */

/* ---------------------------------------------------------------- plug-and-play */

#define MAX_REGISTERED_DRIVERS 16

static const purr_driver_t *s_registered[MAX_REGISTERED_DRIVERS];
static int s_registered_count;

int purr_driver_register(const purr_board_t *board, const purr_driver_t *drv)
{
    if (s_registered_count >= MAX_REGISTERED_DRIVERS) {
        return -1;
    }
    s_registered[s_registered_count++] = drv;

    if (board == NULL) {
        return 0;
    }
    for (int i = 0; i < board->device_count; i++) {
        const purr_device_t *dev = &board->devices[i];
        device_record_t *rec = record_for(dev->name);
        if (rec == NULL || rec->state == PURR_DEV_BOUND) {
            continue;                 /* never steal a device that already has a driver */
        }
        if (strcmp(dev->compatible, drv->compatible) != 0) {
            continue;                 /* not what this device needs -- leave it for bind_device
                                        * to keep reporting its real state (NO_DRIVER etc.) */
        }
        bind_device(rec, dev, board, &drv, 1);
    }
    for (int i = 0; i < s_extra_count; i++) {
        const purr_device_t *dev = s_extra[i];
        device_record_t *rec = record_for(dev->name);
        if (rec == NULL || rec->state == PURR_DEV_BOUND || strcmp(dev->compatible, drv->compatible) != 0) {
            continue;
        }
        bind_device(rec, dev, board, &drv, 1);
    }
    return 0;
}

int purr_device_add(const purr_board_t *board, const purr_device_t *dev)
{
    if (dev == NULL || dev->name == NULL || dev->compatible == NULL || s_extra_count >= MAX_EXTRA_DEVICES) {
        return -1;
    }
    for (int i = 0; i < s_extra_count; i++) {
        if (strcmp(s_extra[i]->name, dev->name) == 0) {
            return -1;                /* declared already */
        }
    }
    if (board != NULL) {
        for (int i = 0; i < board->device_count; i++) {
            if (strcmp(board->devices[i].name, dev->name) == 0) {
                return -1;            /* the board's own table has it */
            }
        }
    }
    device_record_t *rec = record_for(dev->name);
    if (rec == NULL) {
        return -1;
    }
    s_extra[s_extra_count++] = dev;
    bind_device(rec, dev, board, s_registered, s_registered_count);          /* a module already loaded */
    if (rec->state == PURR_DEV_NO_DRIVER && s_boot_table != NULL) {
        bind_device(rec, dev, board, s_boot_table, s_boot_table_count);      /* or one built into the kernel */
    }
    return 0;
}

int purr_driver_registered(const char *compatible)
{
    for (int i = 0; i < s_registered_count; i++) {
        if (strcmp(s_registered[i]->compatible, compatible) == 0) {
            return 1;
        }
    }
    return 0;
}

void purr_drivers_bind_extra(const purr_board_t *board)
{
    if (board == NULL) {
        return;
    }
    for (int i = 0; i < s_extra_count; i++) {
        const purr_device_t *dev = s_extra[i];
        device_record_t *rec = record_for(dev->name);
        if (rec != NULL && rec->state != PURR_DEV_BOUND) {
            bind_device(rec, dev, board, s_registered, s_registered_count);
        }
    }
    for (int i = 0; i < board->device_count; i++) {
        const purr_device_t *dev = &board->devices[i];
        device_record_t *rec = record_for(dev->name);
        if (rec == NULL || rec->state == PURR_DEV_BOUND) {
            continue;
        }
        bind_device(rec, dev, board, s_registered, s_registered_count);
    }
}

void purr_driver_registry_reset(void)
{
    s_registered_count = 0;
    s_net_settled = 0;
}

int purr_drivers_net_is_settled(void)
{
    return s_net_settled;
}

void purr_drivers_net_settled(const purr_board_t *board)
{
    s_net_settled = 1;
    if (board == NULL) {
        return;
    }
    for (int i = 0; i < board->device_count; i++) {
        const purr_device_t *dev = &board->devices[i];
        device_record_t *rec = record_for(dev->name);
        if (rec == NULL || rec->state != PURR_DEV_DEFERRED) {
            continue;
        }
        bind_device(rec, dev, board, s_registered, s_registered_count);          /* a module driver registered while it waited */
        if (rec->state == PURR_DEV_NO_DRIVER && s_boot_table != NULL) {
            bind_device(rec, dev, board, s_boot_table, s_boot_table_count);      /* or one built into the kernel */
        }
    }
}

void purr_driver_unregister(const purr_driver_t *drv)
{
    for (int i = 0; i < s_record_count; i++) {
        if (s_records[i].state == PURR_DEV_BOUND && s_records[i].driver == drv) {
            drv->remove(s_records[i].handle);
            purr_pins_release_owner(s_records[i].name);
            s_records[i].state = PURR_DEV_UNBOUND;
            s_records[i].handle = NULL;
            s_records[i].driver = NULL;
        }
    }
    for (int i = 0; i < s_registered_count; i++) {
        if (s_registered[i] == drv) {
            for (int j = i; j < s_registered_count - 1; j++) {
                s_registered[j] = s_registered[j + 1];
            }
            s_registered_count--;
            break;
        }
    }
}

uint32_t purr_driver_class_id(const purr_driver_t *drv)
{
    if (drv == NULL || (drv->bus_abi != 0 && drv->bus_abi < 6)) {
        return 0;                 /* built before the class fields existed: they are not there to read */
    }
    return drv->class_id;
}

int purr_class_find(uint32_t class_id, purr_class_entry_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < s_record_count; i++) {
        const device_record_t *r = &s_records[i];
        if (r->state != PURR_DEV_BOUND || r->driver == NULL || purr_driver_class_id(r->driver) != class_id ||
            class_id == 0 || r->driver->ops == NULL) {
            continue;
        }
        if (out != NULL && n < max) {
            out[n] = (purr_class_entry_t){.device = r->name, .class_id = class_id, .ops = r->driver->ops, .handle = r->handle};
        }
        n++;
    }
    return n;
}

const void *purr_class_ops(uint32_t class_id, void **handle_out)
{
    purr_class_entry_t e;
    if (purr_class_find(class_id, &e, 1) < 1) {
        return NULL;
    }
    if (handle_out != NULL) {
        *handle_out = e.handle;
    }
    return e.ops;
}

const char *purr_drv_class_name(uint32_t class_id)
{
    switch (class_id) {
    case 0: return "none";
    case 1: return "gnss";
    case 2: return "lora";
    case 3: return "modem";
    case 4: return "wifi";
    case 5: return "hci";
    case 6: return "block";
    case 7: return "ethernet";
    default: return "?";
    }
}

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

const purr_driver_t *purr_device_driver(const char *device_name)
{
    for (int i = 0; i < s_record_count; i++) {
        if (strcmp(s_records[i].name, device_name) == 0) {
            return s_records[i].state == PURR_DEV_BOUND ? s_records[i].driver : NULL;
        }
    }
    return NULL;
}

int purr_device_read(const char *device_name, uint8_t *data, size_t len)
{
    for (int i = 0; i < s_record_count; i++) {
        if (strcmp(s_records[i].name, device_name) == 0) {
            if (s_records[i].state != PURR_DEV_BOUND || s_records[i].driver == NULL ||
                s_records[i].driver->read == NULL) {
                return -1;
            }
            return s_records[i].driver->read(s_records[i].handle, data, len);
        }
    }
    return -1;
}
