/*
 * purr_lan_discover.h - finding an SDK server (`purrstrap dev serve`) on the local network, for the Developer Portal and for
 * recovery. Two ways at once, so it works on a bare LAN, behind a firewall that drops probes, and inside the VM:
 *   - it sends a probe ("PURR-REPO?") to the limited broadcast, the subnet's own broadcast, and the VM host (10.0.2.2), and
 *   - it listens on the discovery port for the announcements a server sends every few seconds on its own.
 * Only a local address may answer (purr_repo.h's rule). The same line carries either: PURR-REPO! <name> <port> <id>.
 * Blocks for wait_ms: call from a worker task.
 */
#ifndef PURR_LAN_DISCOVER_H
#define PURR_LAN_DISCOVER_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char base[80];                      /* http://192.168.1.20:47801 */
    char name[32];
    char id[24];
} purr_lan_server_t;

/* How many distinct servers were heard in wait_ms (up to max). */
int purr_lan_discover(purr_lan_server_t *out, int max, int wait_ms);

#ifdef __cplusplus
}
#endif

#endif /* PURR_LAN_DISCOVER_H */
