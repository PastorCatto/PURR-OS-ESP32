// wince_taskbar.cpp — ported from PURR-OS-0.11/devices/apps/purr_taskbar.cpp verbatim.
#include "wince_taskbar.h"
#include <string.h>

taskbar_entry_t taskbar_entries[TASKBAR_MAX_ENTRIES];
int taskbar_entry_count = 0;
mw_handle_t taskbar_focused_handle = MW_INVALID_HANDLE;

void taskbar_register(mw_handle_t handle, const char *name)
{
    if (taskbar_entry_count >= TASKBAR_MAX_ENTRIES) return;
    taskbar_entries[taskbar_entry_count].handle = handle;
    strncpy(taskbar_entries[taskbar_entry_count].name, name,
            sizeof(taskbar_entries[0].name) - 1);
    taskbar_entries[taskbar_entry_count].name[sizeof(taskbar_entries[0].name) - 1] = '\0';
    taskbar_entry_count++;
    taskbar_focused_handle = handle;
}

void taskbar_unregister(mw_handle_t handle)
{
    if (taskbar_focused_handle == handle)
        taskbar_focused_handle = MW_INVALID_HANDLE;
    for (int i = 0; i < taskbar_entry_count; i++) {
        if (taskbar_entries[i].handle == handle) {
            for (int j = i; j < taskbar_entry_count - 1; j++)
                taskbar_entries[j] = taskbar_entries[j + 1];
            taskbar_entry_count--;
            return;
        }
    }
}

void taskbar_set_focus(mw_handle_t handle)
{
    taskbar_focused_handle = handle;
}
