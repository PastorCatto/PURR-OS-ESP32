/* SPDX-License-Identifier: Apache-2.0
 * The Android 2.2 (Froyo) style shell is licensed under the Apache License, Version 2.0, the licence the Android Open Source Project uses, in respect of that project.
 * The licence text is in UI/android-assets/NOTICE; the rest of PURR OS is under the GNU GPL version 3 (LICENSE). See THIRD_PARTY.md. */
/*
 * purr_android_priv.h - what the pieces of the Android style shell share (plan7.md, "Android shell build-out").
 *
 *   purr_android.c        build, status strip, key strip, banner and busy, orientation, brightness, the shell contract
 *   purr_android_home.c   the app list, home pages, dock, app drawer, options menu
 *   purr_android_shade.c  the notification shade
 *   purr_android_lock.c   the lock screen and the sign-in screen
 *
 * UI service task only, unless a function says otherwise.
 */
#ifndef PURR_ANDROID_PRIV_H
#define PURR_ANDROID_PRIV_H

#include "lvgl.h"

#include "purr_w10m_priv.h"

#define AND_STATUS_H W10M_STATUS_H
#define AND_KEYS_H   W10M_NAV_H

/* ---- purr_android.c */
lv_obj_t *and_strip(void);                          /* the status strip, for the shade's pull-down */
void and_status_notes(int count);                   /* how many notifications the strip shows an icon for */
void and_go_home(void);                             /* the Home key: everything open steps back to the home screen */
void and_back(void);                                /* the Back key */
void and_keys_visible(int on);                      /* the key strip (Menu, Home, Back) on or off: the lock screen takes the whole screen */
void and_menu_key(void);                            /* the Menu key */
int and_signed_in(void);
void and_signed_in_set(void);

/* ---- purr_android_home.c */
void and_home_build(lv_obj_t *screen);
void and_home_layout(void);
void and_home_reload(void);                         /* the installed apps or the pins changed: rebuild what shows them */
void and_home_reset(void);                          /* close the menu and the drawer, show page 0 */
int and_home_back(void);                            /* the Back key's share: closes the menu or the drawer; 1 if one was open */
void and_menu_toggle(void);
int and_menu_is_open(void);
void and_menu_close(void);
int and_drawer_is_open(void);
void and_drawer_toggle(void);
void and_open_app(const char *name, int system);    /* system: 0 an installed app, 1 Settings, 2 the Developer Portal */
void and_wallpaper_apply(void);                     /* the gradient, or the chosen picture when it is in the asset pack */
void and_rotate_next(void);

/* ---- purr_android_shade.c */
void and_shade_build(void);
void and_shade_layout(void);
void and_shade_toggle(void);
void and_shade_close(void);
int and_shade_is_open(void);
void and_shade_add(const char *title, const char *text);
int and_shade_count(void);
/* The i-th notification, newest first: its title and text. 0, or -1 if there is none. */
int and_shade_peek(int i, char *title, size_t tcap, char *text, size_t xcap);
/* Redraws the notification list on the lock screen (called when the notifications change, and when the lock screen shows). */
void and_lock_notes_refresh(void);

/* ---- purr_android_lock.c */
void and_lock_build(void);
void and_lock_layout(void);
void and_lock_show(void);
void and_lock_unlock(void);
int and_lock_is_on(void);
void and_signin_build(void);
void and_signin_layout(void);
void and_signin_back(void);                         /* Back at the sign-in: from the password to the account list */
void and_signin_hidden_check(void);                 /* a session already exists: hide the sign-in screen */

#endif /* PURR_ANDROID_PRIV_H */
