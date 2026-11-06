/* purr_catrt_msg.h - the `messaging` and `contacts` catcalls (purr_catcalls.h, Messaging/SPEC.md): the message store and the address book of the signed-in user, and the registry of
 * messaging service apps. The store is components/coreos/src/purr_msgstore.c, the address book purr_contacts.c; this file puts them behind the permission gates and the app runtime. */
#ifndef PURR_CATRT_MSG_H
#define PURR_CATRT_MSG_H

#ifdef __cplusplus
extern "C" {
#endif

void purr_catrt_msg_init(void);

/* A message arrived for the user: the shell may show a notification (the Android shell does). A weak no-op here. */
void purr_msg_notify(const char *title, const char *text);

#ifdef __cplusplus
}
#endif

#endif
