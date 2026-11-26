/* mini_health.h - tells the PURR bootloader that this boot reached a working state (resets purrcfg's boot_fail_count), as the full OS does in purr_mark_boot_healthy(). */
#ifndef MINI_HEALTH_H
#define MINI_HEALTH_H

void mini_mark_healthy(void);

#endif
