/*
 * purr_catrt_power.h - the `power` catcall and the permission grants behind it (plan10.md).
 *
 * The grants (coreos/purr_perm.c) are kept per signed-in user in /home/<user>/permissions.conf. A dangerous call
 * from an app with no answer yet queues a prompt and returns PURR_POWER_ASK; the shell shows it, and the user's
 * answer reaches the app as PURR_APPEV_PERM.
 */
#ifndef PURR_CATRT_POWER_H
#define PURR_CATRT_POWER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the catcall with the app runtime and the wake hooks with the power manager. Before purr_catrt_start. */
void purr_catrt_power_init(void);

/* The shell registers how a prompt is shown. Called on the asking app's task; the shell hops to its UI task. */
typedef void (*purr_perm_show_fn)(const char *app, int perm, int denials);
void purr_catrt_perm_set_ui(purr_perm_show_fn show);

/* The user's answer to the prompt now showing: PURR_ANSWER_*. If another prompt is waiting it is described in
 * next_* and 1 is returned (the shell shows it); else 0. UI task. */
#define PURR_ANSWER_DENY   0
#define PURR_ANSWER_ONCE   1
#define PURR_ANSWER_ALWAYS 2
#define PURR_ANSWER_NEVER  3
int purr_catrt_perm_answer(int answer, char *next_app, size_t cap, int *next_perm, int *next_denials);

/* Settings and the shell: one app's saved decision, and changing it. state is PURR_PERM_*. */
int purr_catrt_perm_get(const char *app, int perm);
int purr_catrt_perm_put(const char *app, int perm, int state);     /* 0, or -1; saves; a revoke takes effect now */
void purr_catrt_perm_reset(const char *app);

/* The shell: perms [app | reset <app>] | grant <app> <permission> | revoke <app> <permission>. */
struct purr_cli;
int purr_perms_cmd(struct purr_cli *cli, int argc, char **argv);

/* Apps that have decisions on file for the signed-in user, for the Settings page. */
int purr_catrt_perm_apps(char names[][32], int max);

#ifdef __cplusplus
}
#endif

#endif /* PURR_CATRT_POWER_H */
