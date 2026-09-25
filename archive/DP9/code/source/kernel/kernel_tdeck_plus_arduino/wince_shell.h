// wince_shell.h — entry point for the baked-in WinCE desktop shell.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Brings up MiniWin's HAL + window manager and starts the shell's message
// pump task. Call once from app_main() after display/touch/input catcalls
// are registered. Replaces the miniwin .purr module's own task for this
// device — no module wrapper, the shell is linked directly into the kernel.
void wince_shell_start(void);

#ifdef __cplusplus
}
#endif
