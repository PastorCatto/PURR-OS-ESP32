/*
 * purr_netsettle.h - devices that wait for Wi-Fi.
 *
 * A board device flagged PURR_DEFER_NET (the T-Deck Plus LoRa radio) is not given its pins or probed while Wi-Fi starts: both belong to one service,
 * and bringing the two up together at start was too much for the fast RAM. This starts a short task that waits until Wi-Fi has an address (or, with
 * no network to join, a fixed time) and then lets those devices bind (purr_drivers_net_settled). Call once, after purr_net_init.
 */
#ifndef PURR_NETSETTLE_H
#define PURR_NETSETTLE_H

#ifdef __cplusplus
extern "C" {
#endif

void purr_net_settle_start(void);

#ifdef __cplusplus
}
#endif

#endif
