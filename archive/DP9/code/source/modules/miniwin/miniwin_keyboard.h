#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Polls all registered input catcalls for key events and posts
// MW_KEY_PRESSED_MESSAGE to the currently focused MiniWin window.
// Call once per tick from the miniwin task loop (after mw_process_message).
void miniwin_keyboard_poll(void);

#ifdef __cplusplus
}
#endif
