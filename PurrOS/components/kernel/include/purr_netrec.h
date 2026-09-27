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

#define PURR_NETREC_SSID_LEN 33
#define PURR_NETREC_PASS_LEN 65

/* Best effort: failures are logged, never fatal to the caller (saving this is a convenience,
 * not the primary save, which is /etc/wifi through purr_wifi). */
void purr_netrec_save(const char *ssid, const char *pass);

/* Fills ssid and pass and returns 0 if a valid record is there, else returns nonzero and
 * leaves both as empty strings. */
int purr_netrec_load(char ssid[PURR_NETREC_SSID_LEN], char pass[PURR_NETREC_PASS_LEN]);

#endif /* PURR_NETREC_H */
