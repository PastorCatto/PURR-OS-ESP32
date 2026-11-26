/* purr_catrt_radio.h - the `radio` catcall (purr_catcalls.h): one app at a time holds the LoRa radio, whatever chip it is. */
#ifndef PURR_CATRT_RADIO_H
#define PURR_CATRT_RADIO_H

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the catcall with the app runtime. Once, before apps run (after purr_catrt_power_init, whose prompt it uses). */
void purr_catrt_radio_init(void);

/* The system side (Messages' services screen): who holds the radio, and a request to hand it back. 1 and the name, or 0 when it is free. */
int purr_radio_dev_holder(char *app, unsigned cap);
/* Asks the holder to yield (it gets "radio.yield"); if it has not released within ms, the lease is taken back and it gets PURR_RADIO_LOST. 0, or -1 if nobody holds it. */
int purr_radio_dev_take_back(unsigned ms);

#ifdef __cplusplus
}
#endif

#endif
