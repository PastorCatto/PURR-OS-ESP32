/* mini_link.h - the remote display client (RemoteDisplay/SPEC.md): finds a PURR host on the network, opens a session and draws what it sends.
 * NOT YET: pairing and encryption (spec section 4). Until they exist this link is open, and the host it reconnects to is simply the one it used last. */
#ifndef MINI_LINK_H
#define MINI_LINK_H

#include <stdbool.h>

#define MINI_LINK_LOST 0                 /* a session ran and the host went away or ended it */
#define MINI_LINK_MENU 1                 /* the user held the top right corner during a session: back to the iPod home */
#define MINI_LINK_NOHOST 2               /* no host answered in time, or the user tapped Cancel */

bool mini_link_have_last(void);          /* a host this Mini has used before */
/* Quiet: tries the host used last time first, then asks the network, for up to timeout_ms, with a small "Connecting" screen (Cancel works). If one answers it runs the session. */
int mini_link_reconnect(int timeout_ms);
/* The searching screen with a Cancel button: looks until a host answers (then runs the session) or the user cancels. */
int mini_link_search(void);

#endif
