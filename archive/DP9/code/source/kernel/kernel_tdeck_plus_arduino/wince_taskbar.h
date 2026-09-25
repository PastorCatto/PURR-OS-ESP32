// wince_taskbar.h — running-window registry for the baked-in WinCE shell.
// Ported from PURR-OS-0.11/devices/apps/purr_taskbar.{h,cpp} verbatim.
#pragma once
#include "miniwin.h"

#define TASKBAR_MAX_ENTRIES 8

typedef struct {
    mw_handle_t handle;
    char name[12];
} taskbar_entry_t;

#ifdef __cplusplus
extern "C" {
#endif

extern taskbar_entry_t taskbar_entries[TASKBAR_MAX_ENTRIES];
extern int taskbar_entry_count;
extern mw_handle_t taskbar_focused_handle;

void taskbar_register(mw_handle_t handle, const char *name);
void taskbar_unregister(mw_handle_t handle);
void taskbar_set_focus(mw_handle_t handle);

#ifdef __cplusplus
}
#endif
