/* "notes": a UI-kind app that saves state and is suspended (not run) when it leaves the front. */
#define APP_NAME "notes"
#define APP_ENTRY notes_entry
#define APP_KIND 2
#define APP_FLAGS 0x02   /* saves-state */
#define APP_MEMORY 24576
#include "app_common.h"
