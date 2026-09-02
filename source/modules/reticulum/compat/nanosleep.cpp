// nanosleep.cpp — POSIX nanosleep(), backed by FreeRTOS vTaskDelay().
// vendor/Utilities/OS.h's own non-Arduino OS::sleep(float) calls
// ::nanosleep() directly (declared by newlib's <time.h>, confirmed live:
// this exact ESP-IDF/newlib build has no component that actually
// implements it — a link failure, not a missing header) — reached for
// real from RNS::Transport::inbound()/outbound(), not a rare code path.
// Millisecond granularity here (losing sub-ms precision from the
// timespec) is fine: FreeRTOS's own tick period is generally >= 1ms
// anyway, and nothing in the vendored tree's use of OS::sleep() needs
// finer timing than that.

#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" int nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    if (!req) return -1;

    uint32_t ms = (uint32_t)req->tv_sec * 1000u + (uint32_t)(req->tv_nsec / 1000000);
    vTaskDelay(pdMS_TO_TICKS(ms));
    return 0;
}
