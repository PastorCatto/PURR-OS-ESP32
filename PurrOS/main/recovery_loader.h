/* The recovery loader's entry point (minimal profile). See RecoveryLoader/SPEC.md. */
#ifndef PURR_RECOVERY_LOADER_H
#define PURR_RECOVERY_LOADER_H

/* Never returns: it restarts the device, one way or another. */
void purr_recovery_loader_main(void);

#endif
