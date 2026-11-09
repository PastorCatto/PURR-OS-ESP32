/* purr_notify.h - the shell-neutral side of notifications: a short log of the newest ones, and the hook each shell fills in. A notification raised by the OS (a message arrived)
 * comes in on any task; it is handed to the UI task, added to the log, and the shell is told so it can draw it in its own style (the Android shade and lock screen rows in the
 * Froyo look, the Windows 10 Mobile lock screen lines in the Lumia look). */
#ifndef PURR_NOTIFY_H
#define PURR_NOTIFY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_NOTE_MAX 8

/* Call from the UI task. Adds a notification (newest first) and tells the shell. */
void purr_notify_add(const char *title, const char *text);
int purr_notify_count(void);
int purr_notify_get(int i, char *title, size_t tcap, char *text, size_t xcap);      /* newest first; 0 or -1 if there is none */
void purr_notify_clear(void);

/* Implemented by the shell that is compiled in (weak no-op here): a notification was added. Runs on the UI task. */
void purr_shell_notified(const char *title, const char *text);

#ifdef __cplusplus
}
#endif

#endif
