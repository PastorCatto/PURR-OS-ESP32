/*
 * kspike_module.c: the freestanding-CoreOS spike's one test module (CoreOSSpike/SPEC.md).
 * Throwaway. Calls only through purr_kspike_table_t -- no libc, no FreeRTOS linkage of its
 * own, same freestanding discipline every real module already follows, just exercising much
 * heavier calls (a real heap allocation, a real FreeRTOS task it becomes the entry point of).
 */
#include "purr_kspike_abi.h"

typedef struct {
    purr_cli_t *cli;
    const purr_kspike_table_t *t;
    volatile uint32_t iterations;
    volatile uint32_t errors;
    volatile int done;
} kspike_state_t;

/* Runs as its OWN FreeRTOS task, not the calling task -- the actual thing being tested: this
 * function is relocated PSRAM code, and it becomes a real task's entry point. */
static void spike_task_entry(void *arg)
{
    kspike_state_t *st = (kspike_state_t *)arg;
    const purr_kspike_table_t *t = st->t;

    for (uint32_t i = 0; i < 20; i++) {
        uint32_t want = i * 7 + 1;
        t->reg_write(want);
        uint32_t got = t->reg_read();
        if (got != want) {
            st->errors++;
        }
        t->printf(st->cli, "kspike: task iter %u, reg=%u\n", (unsigned)i, (unsigned)got);
        st->iterations = i + 1;
        t->task_delay(50);
    }
    st->done = 1;
    t->task_exit();   /* must never just return */
}

__attribute__((used))
void kspike_module_entry(const purr_kspike_table_t *table, purr_cli_t *cli)
{
    table->puts(cli, "kspike: starting\n");

    /* Real heap, through the table: allocate, write a pattern, read it back. */
    uint8_t *buf = (uint8_t *)table->heap_alloc(64);
    int heap_ok = 0;
    if (buf) {
        for (int i = 0; i < 64; i++) {
            buf[i] = (uint8_t)(i ^ 0x5a);
        }
        heap_ok = 1;
        for (int i = 0; i < 64; i++) {
            if (buf[i] != (uint8_t)(i ^ 0x5a)) {
                heap_ok = 0;
                break;
            }
        }
    }
    table->printf(cli, "kspike: heap alloc/write/read %s\n", heap_ok ? "OK" : "FAIL");

    kspike_state_t *st = (kspike_state_t *)table->heap_alloc(sizeof(kspike_state_t));
    if (st == NULL) {
        table->puts(cli, "kspike: FAIL (state alloc)\n");
        if (buf) table->heap_free(buf);
        return;
    }
    st->cli = cli;
    st->t = table;
    st->iterations = 0;
    st->errors = 0;
    st->done = 0;

    /* The real test: a new FreeRTOS task whose entry point is our own relocated code. */
    int created = table->task_create(spike_task_entry, "kspike", 2048, st);
    table->printf(cli, "kspike: task_create %s\n", created ? "OK" : "FAIL");

    int waited_ms = 0;
    while (created && !st->done && waited_ms < 5000) {
        table->task_delay(100);
        waited_ms += 100;
    }

    table->printf(cli, "kspike: iterations=%u errors=%u done=%d\n",
                 (unsigned)st->iterations, (unsigned)st->errors, st->done);

    if (heap_ok && created && st->done && st->errors == 0 && st->iterations == 20) {
        table->puts(cli, "kspike: PASS\n");
    } else {
        table->puts(cli, "kspike: FAIL\n");
    }

    table->heap_free(st);
    if (buf) {
        table->heap_free(buf);
    }
}
