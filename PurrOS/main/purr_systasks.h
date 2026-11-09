/*
 * purr_systasks.h - the system's own tasks (Wi-Fi, the drivers' helpers, the install and Store jobs, LVGL...) with the fast RAM their stacks hold,
 * for the Task Manager's "System services" part. Apps are listed by the runtime (purr_catrt_tasks); this is everything else that is running.
 * Needs CONFIG_FREERTOS_USE_TRACE_FACILITY.
 */
#ifndef PURR_SYSTASKS_H
#define PURR_SYSTASKS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char name[16];
    uint32_t stack;             /* bytes the stack takes from the heap it was allocated in; 0 when that is not known (a static stack) */
    uint32_t spare;             /* the most stack it has left at its tightest, in bytes */
    uint8_t blocked;            /* 1 waiting, 0 running or ready */
    uint8_t internal;           /* 1 when the stack is in internal (fast) RAM */
} purr_systask_t;

/* Fills `out` with up to `max` tasks, the biggest stacks first. Returns how many. Takes a moment (it lists every task): call from the UI at a slow pace. */
int purr_systasks(purr_systask_t *out, int max);

/* Over every task whose stack is in fast RAM: the bytes the stacks take, the part of them never used, and the size of the whole fast heap. */
void purr_systasks_totals(uint32_t *stacks, uint32_t *spare, uint32_t *heap_total);

/* Fast RAM: free now, the biggest block that is free, and the least that has been free since start. */
void purr_systasks_fast_ram(uint32_t *free_now, uint32_t *largest, uint32_t *lowest);

#ifdef __cplusplus
}
#endif

#endif
