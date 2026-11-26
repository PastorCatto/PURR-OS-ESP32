/* mini_setup.h - remote setup from the Mini PDA desktop (plan1.md, "Remote setup"): find a device asking for setup, pair with it, then send it a name, its first account, a Wi-Fi network
 * and the apps it should have, and finish. The messages are purr_px_setup.h's, on the setup channel of a secure session. */
#ifndef MINI_SETUP_H
#define MINI_SETUP_H

/* Runs the whole thing until the person finishes or goes back. */
void mini_setup_run(void);

#endif
