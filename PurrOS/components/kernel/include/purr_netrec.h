/*
 * purr_netrec.h - the recovery network record (RecoveryLoader/SPEC.md section 3).
 *
 * A small raw record, in its own partition, holding the last network the running system
 * connected to: a network name and password in plain form, exactly like /etc/wifi. Real
 * protection needs flash encryption, a later step. The recovery loader has no filesystem,
 * so it reads this directly instead of /etc/wifi.
 */
#ifndef PURR_NETREC_H
#define PURR_NETREC_H

#include <stdint.h>

#define PURR_NETREC_SSID_LEN 33
#define PURR_NETREC_PASS_LEN 65

/* Best effort: failures are logged, never fatal to the caller (saving this is a convenience,
 * not the primary save, which is /etc/wifi through purr_wifi). */
void purr_netrec_save(const char *ssid, const char *pass);

/* Fills ssid and pass and returns 0 if a valid record is there, else returns nonzero and
 * leaves both as empty strings. */
int purr_netrec_load(char ssid[PURR_NETREC_SSID_LEN], char pass[PURR_NETREC_PASS_LEN]);

/* The recovery source: the SDK server (a base address such as http://192.168.1.20:47801) the owner chose on the recovery setup
 * page, with its name and id, so the next boot pulls from the same place. Stored beside the network record. save with an empty
 * base clears it. load returns 0 if one is saved. */
#define PURR_NETREC_SRC_BASE_LEN 80
#define PURR_NETREC_SRC_NAME_LEN 32
#define PURR_NETREC_SRC_ID_LEN 24
void purr_netrec_save_source(const char *base, const char *name, const char *id);
int purr_netrec_load_source(char base[PURR_NETREC_SRC_BASE_LEN], char name[PURR_NETREC_SRC_NAME_LEN],
                            char id[PURR_NETREC_SRC_ID_LEN]);

/* The official source: where the OS update check asks for recovery.manifest and modules.manifest when a signed move file (purr_move.h) has told the device the
 * official location changed. Stored in the same sector (the recovery loader and KittenOS have no file system and read it from here too). `seq` is the number of the
 * last move followed: it is kept when the source is cleared (back to the compiled default), so an old move file cannot be replayed. save with empty addresses clears
 * the source and keeps seq. load returns 0 if a record is there (the addresses are empty when it has been cleared), -1 if there was never one (seq 0). */
#define PURR_NETREC_OFF_URL_LEN 120
#define PURR_NETREC_OFF_MSG_LEN 80
void purr_netrec_save_official(const char *recovery, const char *modules, const char *message, uint32_t seq);
int purr_netrec_load_official(char recovery[PURR_NETREC_OFF_URL_LEN], char modules[PURR_NETREC_OFF_URL_LEN], char message[PURR_NETREC_OFF_MSG_LEN], uint32_t *seq);

#endif /* PURR_NETREC_H */
