/*
 * purr_catrt_ui.h - the `ui` catcall's window host (plan9.md). UI service task only, except purr_catrt_ui_init.
 */
#ifndef PURR_CATRT_UI_H
#define PURR_CATRT_UI_H

#include "lvgl.h"

/* Registers the `ui` catcall with the app runtime. Call once, before apps run. */
void purr_catrt_ui_init(void);

/* Re-lays out every app window (the screen rotated). UI task. */
void purr_catrt_ui_layout(void);

/* The Back key: the window that is on screen goes back or closes, as its header does. 1 if there was one. UI task. */
int purr_catrt_ui_back(void);

/* 1 while an app's window is on screen (a shell leaves the keys to it). UI task. */
int purr_catrt_ui_is_open(void);
/* The sheet of the window in front, for a shell that moves a highlight over its controls; NULL when none is open. UI task. */
lv_obj_t *purr_catrt_ui_front(void);

/* Scrolls the window that is on screen, if an app's window is: dy > 0 shows what is further down. 1 if there was one to scroll. UI task. */
int purr_catrt_ui_scroll(int dy);

#endif /* PURR_CATRT_UI_H */
