/*
 * purr_radio_bars.h - the status bar's "service" bars, taken from the LoRa radio.
 *
 * There is no modem on this hardware. The service icon shows how well the LoRa radio heard its last packet (any packet; which network, such as a
 * mesh node, is for later): purr_si_lora_level() in coreos holds the rule. Reads only what the driver kept, so it never touches the chip and never
 * disturbs an app that is using the radio. Safe from any task.
 */
#ifndef PURR_RADIO_BARS_H
#define PURR_RADIO_BARS_H

#ifdef __cplusplus
extern "C" {
#endif

/* -1 when there is no radio (no driver, or a driver from before the status call); otherwise 0 (nothing heard lately) to 4 bars. */
int purr_radio_bars(void);

#ifdef __cplusplus
}
#endif

#endif
