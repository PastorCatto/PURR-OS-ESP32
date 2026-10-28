/*
 * oobe_firstboot.h - part 2 of the internet-recovery setup wizard (purr_oobe.h): KittenOS's first boot after the recovery
 * loader restored it. Pick a colour, create the account, date and time, done; the rest of the system (stage 2) installs in the
 * background while the user answers.
 */
#ifndef OOBE_FIRSTBOOT_H
#define OOBE_FIRSTBOOT_H

#ifdef __cplusplus
extern "C" {
#endif

/* 1 if this boot should run the wizard: a colour T-Deck, KittenOS, and no account on the device yet. */
int oobe_firstboot_available(void);

/* 1 if a reinstall that kept the data is carrying on (an account exists, stage 2 pending): the "just a few more minutes" screen instead
 * of the text installer. Run it with oobe_firstboot_run(). */
int oobe_firstboot_finishing_available(void);

/* Runs the wizard. It does not return unless LVGL could not start, in which case the caller carries on with the text setup. */
void oobe_firstboot_run(void);

#ifdef __cplusplus
}
#endif

#endif /* OOBE_FIRSTBOOT_H */
