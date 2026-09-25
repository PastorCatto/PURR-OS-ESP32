#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Entry point registered with system_task as PURR_HAS_CLASSIC_MAC shell.
// Initialises drv_umac, lib_purr_ipc, wires touch and display, then
// launches the emulator task and IPC dispatch task.
void purr_classic_start(void);

#ifdef __cplusplus
}
#endif
