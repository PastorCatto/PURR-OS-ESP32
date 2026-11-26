/*
 * purr_rd_host.h - the remote display host (RemoteDisplay/SPEC.md): serves this device's screen to a PURR OS Mini (or any client that speaks the protocol) and takes its touches.
 *
 * Shell-independent: the LVGL flush callback (purr_lvgl.c) tells this file every rectangle that was redrawn, so whatever shell is running is what the client sees.
 * OFF unless the setting `remote.display` is on, and not yet encrypted or paired (spec section 4 is still to do): until it is, "on" means any display on the same network may look at this screen.
 */
#ifndef PURR_RD_HOST_H
#define PURR_RD_HOST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* From the LVGL flush callback (UI task), for every rectangle sent to the panel (panel coordinates, host-endian RGB565, w * h pixels packed): copies it for a connected client; costs
 * one flag test when there is none. */
void purr_rd_host_flush(int x, int y, int w, int h, const uint16_t *px);

/* The setting `remote.display`: on a real device off until it is turned on (Settings, System, "Remote display"), on by default inside a VM. */
int purr_rd_host_enabled(void);
void purr_rd_host_set_enabled(int on);

/* Registers the setting and starts the service task (its stack in PSRAM). Safe to call once, after the display is up. */
void purr_rd_host_start(void);

#ifdef __cplusplus
}
#endif

#endif
