/*
 * purr_catrt_ui.h - the `ui` catcall's window host (plan9.md). UI service task only, except purr_catrt_ui_init.
 */
#ifndef PURR_CATRT_UI_H
#define PURR_CATRT_UI_H

/* Registers the `ui` catcall with the app runtime. Call once, before apps run. */
void purr_catrt_ui_init(void);

/* Re-lays out every app window (the screen rotated). UI task. */
void purr_catrt_ui_layout(void);

#endif /* PURR_CATRT_UI_H */
