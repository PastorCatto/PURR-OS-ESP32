/* purr_lan_discover.c - see purr_lan_discover.h. */
#include "purr_lan_discover.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "purr_repo.h"

static const char *TAG = "lan";

#define DISCOVERY_PORT 47800

static int open_probe_socket(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s >= 0) {
        int yes = 1;
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    }
    return s;
}

/* The listener for announcements: bound to the discovery port (shared, so another listener on this device does not stop us). */
static int open_listener(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        return -1;
    }
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(DISCOVERY_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(s);
        return -1;                                      /* a missed announcement is not an error: the probe still works */
    }
    return s;
}

/* The broadcast address of the network the device is on: ip | ~netmask. 0 if it has no address yet. */
static uint32_t subnet_broadcast(void)
{
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (nif == NULL || esp_netif_get_ip_info(nif, &ip) != ESP_OK || ip.ip.addr == 0) {
        return 0;
    }
    return (ip.ip.addr | ~ip.netmask.addr);              /* network byte order, as sendto wants it */
}

static void send_probe(int s, uint32_t addr_be)
{
    static const char probe[] = "PURR-REPO?";
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(DISCOVERY_PORT), .sin_addr.s_addr = addr_be};
    sendto(s, probe, sizeof(probe) - 1, 0, (struct sockaddr *)&to, sizeof(to));
}

/* Reads one datagram if there is one; adds the server if it is a new, local one. */
static void take(int s, purr_lan_server_t *out, int max, int *found)
{
    char buf[128];
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    int n = recvfrom(s, buf, sizeof(buf) - 1, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
    if (n <= 0) {
        return;
    }
    buf[n] = '\0';
    uint32_t ip = ntohl(from.sin_addr.s_addr);
    if ((ip >> 24) == 127) {
        ip = 0x0A000202u;           /* the VM's user-mode network hands a host reply back as loopback: the host is 10.0.2.2 */
    }
    char name[32], id[24];
    unsigned port;
    if (!purr_repo_ip_is_local(ip) || sscanf(buf, "PURR-REPO! %31s %u %23s", name, &port, id) != 3 || port == 0 ||
        port > 65535) {
        return;                                          /* not ours, or not local: ignored */
    }
    purr_lan_server_t sv;
    snprintf(sv.name, sizeof(sv.name), "%s", name);
    snprintf(sv.id, sizeof(sv.id), "%s", id);
    snprintf(sv.base, sizeof(sv.base), "http://%u.%u.%u.%u:%u", (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 255),
             (unsigned)((ip >> 8) & 255), (unsigned)(ip & 255), port);
    for (int k = 0; k < *found; k++) {
        if (strcmp(out[k].base, sv.base) == 0) {
            return;                                      /* heard already (a probe reply and an announcement) */
        }
    }
    if (*found < max) {
        out[(*found)++] = sv;
    }
}

int purr_lan_discover(purr_lan_server_t *out, int max, int wait_ms)
{
    if (max <= 0) {
        return 0;
    }
    int probe = open_probe_socket();
    int listen_s = open_listener();
    if (probe < 0 && listen_s < 0) {
        return 0;
    }
    if (probe >= 0) {
        send_probe(probe, htonl(INADDR_BROADCAST));
        uint32_t sub = subnet_broadcast();
        if (sub != 0 && sub != htonl(INADDR_BROADCAST)) {
            send_probe(probe, sub);
        }
        send_probe(probe, inet_addr("10.0.2.2"));        /* the VM's host */
    }
    int found = 0;
    for (int waited = 0; waited < wait_ms && found < max; waited += 100) {
        fd_set rd;
        FD_ZERO(&rd);
        int top = -1;
        if (probe >= 0) {
            FD_SET(probe, &rd);
            top = probe;
        }
        if (listen_s >= 0) {
            FD_SET(listen_s, &rd);
            if (listen_s > top) {
                top = listen_s;
            }
        }
        struct timeval tv = {.tv_sec = 0, .tv_usec = 100 * 1000};
        if (select(top + 1, &rd, NULL, NULL, &tv) <= 0) {
            continue;
        }
        if (probe >= 0 && FD_ISSET(probe, &rd)) {
            take(probe, out, max, &found);
        }
        if (listen_s >= 0 && FD_ISSET(listen_s, &rd)) {
            take(listen_s, out, max, &found);
        }
    }
    if (probe >= 0) {
        close(probe);
    }
    if (listen_s >= 0) {
        close(listen_s);
    }
    ESP_LOGI(TAG, "heard %d server(s)", found);
    return found;
}
