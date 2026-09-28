/*
 * purr_kspike_abi.h - SPIKE ONLY (CoreOSSpike/SPEC.md). A separate call table from the real
 * purr_core_table_t, used by exactly one throwaway test module to find out whether a
 * freestanding, relocated PSRAM blob can drive real FreeRTOS/heap calls through a table, not
 * just puts/printf. Never touched by the real four modules or the real loader. Delete this
 * file, its loader in commands.c, and CoreOSSpike/ once the spike's result is recorded.
 */
#ifndef PURR_KSPIKE_ABI_H
#define PURR_KSPIKE_ABI_H

#include <stddef.h>
#include <stdint.h>

#include "purr_cli.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_KSPIKE_ABI_VERSION 1u

typedef struct {
    void (*puts)(purr_cli_t *cli, const char *s);
    void (*printf)(purr_cli_t *cli, const char *fmt, ...);

    /* Real heap, not a stub -- CoreOS calls malloc/free directly today; this is the same
     * thing, reached through a table instead, since a freestanding blob has no libc linkage
     * to call malloc by name itself. */
    void *(*heap_alloc)(size_t n);
    void (*heap_free)(void *p);

    /* The actual thing this spike exists to test: can relocated PSRAM code become a real
     * FreeRTOS task's entry point, not just run synchronously on the calling task? `entry` is
     * itself a function living in the relocated blob. Returns 1 on success. */
    int (*task_create)(void (*entry)(void *arg), const char *name, uint32_t stack_words, void *arg);
    void (*task_delay)(uint32_t ms);
    /* A spawned task must never return; it calls this instead of returning (wraps
     * vTaskDelete(NULL)). */
    void (*task_exit)(void);

    /* A synthetic "driver": a fixed in-memory word standing in for a real hardware register,
     * proving the table pattern generalizes past heap/scheduler to arbitrary hardware access
     * without needing a real driver for a throwaway spike. */
    uint32_t (*reg_read)(void);
    void (*reg_write)(uint32_t v);
} purr_kspike_table_t;

/* Runs synchronously on the calling task; spawns its own task via table->task_create and
 * waits for it. Returns nothing -- it reports PASS/FAIL over `cli` itself. */
typedef void (*purr_kspike_entry_fn)(const purr_kspike_table_t *table, purr_cli_t *cli);

#ifdef __cplusplus
}
#endif

#endif /* PURR_KSPIKE_ABI_H */
