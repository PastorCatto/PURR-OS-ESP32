/*
 * purr_contacts.h - the address book behind the `contacts` catcall (Messaging/SPEC.md): a person, with one address on each messaging service they use.
 * Kept as plain text, one line per person: "C|<id>|<favourite 0/1>|<name>|<service>=<address>;<service>=<address>". Plain C, no ESP-IDF, host-tested.
 */
#ifndef PURR_CONTACTS_H
#define PURR_CONTACTS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_CT_MAX    64
#define PURR_CT_NAME   24
#define PURR_CT_ADDRS  4
#define PURR_CT_SVC    16       /* a service key, the same as the message store's: lower case letters and digits */
#define PURR_CT_ADDR   40

typedef struct {
    char service[PURR_CT_SVC];
    char addr[PURR_CT_ADDR];
} purr_ct_addr_t;

typedef struct {
    uint32_t id;                /* stable; 0 = unused */
    char name[PURR_CT_NAME];
    uint8_t fav;
    uint8_t naddr;
    purr_ct_addr_t a[PURR_CT_ADDRS];
} purr_contact_t;

typedef struct {
    purr_contact_t c[PURR_CT_MAX];
    int n;
    uint32_t next_id;
} purr_contacts_t;

void purr_ct_init(purr_contacts_t *b);
/* New contact; returns its id, or < 0 (full, empty name). The characters | ; = and line breaks are replaced by spaces. */
int32_t purr_ct_add(purr_contacts_t *b, const char *name);
int purr_ct_rename(purr_contacts_t *b, uint32_t id, const char *name);
int purr_ct_set_fav(purr_contacts_t *b, uint32_t id, int fav);
/* Sets the person's address on a service (replacing the one there). 0, or < 0 (unknown id, bad service key, no room). */
int purr_ct_set_addr(purr_contacts_t *b, uint32_t id, const char *service, const char *addr);
int purr_ct_remove_addr(purr_contacts_t *b, uint32_t id, const char *service);
int purr_ct_remove(purr_contacts_t *b, uint32_t id);
const purr_contact_t *purr_ct_get(const purr_contacts_t *b, uint32_t id);
/* The contact that has this address on this service: its id, or 0. */
uint32_t purr_ct_find_addr(const purr_contacts_t *b, const char *service, const char *addr);
/* Ids sorted by name, ignoring case; only those whose name contains `filter` (any case) when it is not NULL or empty. Returns how many were written. */
int purr_ct_order(const purr_contacts_t *b, uint32_t *ids, int max, const char *filter);

size_t purr_ct_format(const purr_contacts_t *b, char *out, size_t cap);
void purr_ct_parse(purr_contacts_t *b, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* PURR_CONTACTS_H */
