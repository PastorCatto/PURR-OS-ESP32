/* purr_systasks.c - see purr_systasks.h. */
#include "purr_systasks.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SIZE_CACHE 48

/* heap_caps_get_allocated_size() asserts on an address that is in no heap (a task's static stack), so the address is checked first, and the answer is kept: a
 * stack never changes size, and the check walks the heap. */
static struct {
    const void *base;
    uint32_t size;
} s_sizes[SIZE_CACHE];
static int s_nsizes;

static uint32_t stack_bytes(const void *base)
{
    for (int i = 0; i < s_nsizes; i++) {
        if (s_sizes[i].base == base) {
            return s_sizes[i].size;
        }
    }
    uint32_t size = 0;
    if (heap_caps_check_integrity_addr((intptr_t)base, false)) {
        size = (uint32_t)heap_caps_get_allocated_size((void *)base);
    }
    if (s_nsizes < SIZE_CACHE) {
        s_sizes[s_nsizes].base = base;
        s_sizes[s_nsizes].size = size;
        s_nsizes++;
    }
    return size;
}

int purr_systasks(purr_systask_t *out, int max)
{
    UBaseType_t cap = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t *st = heap_caps_malloc(cap * sizeof(*st), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (st == NULL) {
        return 0;
    }
    UBaseType_t n = uxTaskGetSystemState(st, cap, NULL);
    int k = 0;
    for (UBaseType_t i = 0; i < n; i++) {
        purr_systask_t t;
        memset(&t, 0, sizeof(t));
        strncpy(t.name, st[i].pcTaskName, sizeof(t.name) - 1);
        t.stack = stack_bytes(st[i].pxStackBase);
        t.spare = (uint32_t)st[i].usStackHighWaterMark * sizeof(StackType_t);
        t.blocked = st[i].eCurrentState == eBlocked || st[i].eCurrentState == eSuspended;
        t.internal = esp_ptr_internal(st[i].pxStackBase) ? 1 : 0;
        int at = k < max ? k : max;                     /* insertion sort, the biggest stack first, and the list is capped */
        while (at > 0 && (out[at - 1].stack < t.stack || (out[at - 1].stack == t.stack && strcmp(out[at - 1].name, t.name) > 0))) {
            if (at < max) {
                out[at] = out[at - 1];
            }
            at--;
        }
        if (at < max) {
            out[at] = t;
            if (k < max) {
                k++;
            }
        }
    }
    free(st);
    return k;
}

void purr_systasks_totals(uint32_t *stacks, uint32_t *spare, uint32_t *heap_total)
{
    *stacks = *spare = 0;
    *heap_total = (uint32_t)heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    UBaseType_t cap = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t *st = heap_caps_malloc(cap * sizeof(*st), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (st == NULL) {
        return;
    }
    UBaseType_t n = uxTaskGetSystemState(st, cap, NULL);
    for (UBaseType_t i = 0; i < n; i++) {
        if (esp_ptr_internal(st[i].pxStackBase)) {
            *stacks += stack_bytes(st[i].pxStackBase);
            *spare += (uint32_t)st[i].usStackHighWaterMark * sizeof(StackType_t);
        }
    }
    free(st);
}

void purr_systasks_fast_ram(uint32_t *free_now, uint32_t *largest, uint32_t *lowest)
{
    *free_now = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    *largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    *lowest = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
