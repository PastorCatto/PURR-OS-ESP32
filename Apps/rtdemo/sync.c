/* "sync": a daemon. It starts in the background and never takes the foreground. */
#define APP_NAME "sync"
#define APP_ENTRY sync_entry
#define APP_KIND 1
#define APP_FLAGS 0x01   /* background-ok */
#define APP_MEMORY 8192
#include "app_common.h"
