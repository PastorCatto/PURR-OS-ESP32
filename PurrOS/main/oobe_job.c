/* oobe_job.c - see oobe_job.h. */
#include "oobe_job.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include "freertos/task.h"

/* Task stacks come from INTERNAL RAM, one contiguous block each, and with Wi-Fi up on the real device that is scarce (the VM
 * uses wired Ethernet and has plenty): a 40 KB stack for every job could not be allocated there, so account creation and every
 * recovery action failed with "couldn't start that". So each job asks only for what it needs. Measured in the VM (high-water mark):
 * the restore used about 13 KB, stage 2 (kernel and modules) about 22 KB; 10 KB and 20 KB overflowed. */

enum { JOB_IDLE, JOB_RUNNING, JOB_OK, JOB_FAILED };

struct oobe_job {
    volatile int state;
    volatile int per_mille;
    char msg[80];
    SemaphoreHandle_t mu;
    oobe_job_fn fn;
    void *arg;
    unsigned stack;
};

oobe_job_t *oobe_job_new(void)
{
    oobe_job_t *j = calloc(1, sizeof(*j));
    if (j != NULL) {
        j->mu = xSemaphoreCreateMutex();
        j->per_mille = -1;
    }
    return j;
}

/* The "host": the main task, which has a 32 KB stack and nothing to do while the wizard is up. Install-sized jobs run on it, so they
 * need no new internal RAM at all (the device never had a 32 KB block free to give them). One job at a time; anything else still gets its
 * own small task. */
static TaskHandle_t s_host;
static QueueHandle_t s_hostq;
static volatile int s_host_busy;
static jmp_buf s_host_jb;

static void finish_log(oobe_job_t *j, int r, unsigned stack)
{
    ESP_LOGI("oobe-job", "finished (%s); least stack left over: %u of %u bytes", r == 0 ? "ok" : "failed",
             (unsigned)uxTaskGetStackHighWaterMark(NULL), stack);
}

void oobe_job_host_run(void)
{
    s_hostq = xQueueCreate(1, sizeof(oobe_job_t *));
    s_host = xTaskGetCurrentTaskHandle();
    for (;;) {
        oobe_job_t *j = NULL;
        if (xQueueReceive(s_hostq, &j, portMAX_DELAY) != pdTRUE || j == NULL) {
            continue;
        }
        if (setjmp(s_host_jb) == 0) {
            int r = j->fn(j, j->arg);
            finish_log(j, r, CONFIG_ESP_MAIN_TASK_STACK_SIZE);
            j->state = r == 0 ? JOB_OK : JOB_FAILED;
        } else {
            finish_log(j, 1, CONFIG_ESP_MAIN_TASK_STACK_SIZE);        /* it called oobe_job_abort(): the state and message are set */
        }
        s_host_busy = 0;
    }
}

static void worker(void *p)
{
    oobe_job_t *j = p;
    int r = j->fn(j, j->arg);
    finish_log(j, r, j->stack);
    j->state = r == 0 ? JOB_OK : JOB_FAILED;
    vTaskDelete(NULL);
}

int oobe_job_start_stack(oobe_job_t *job, oobe_job_fn fn, void *arg, unsigned stack_bytes)
{
    if (job == NULL || job->state == JOB_RUNNING) {
        return -1;
    }
    job->fn = fn;
    job->arg = arg;
    job->stack = stack_bytes;
    job->per_mille = -1;
    xSemaphoreTake(job->mu, portMAX_DELAY);
    job->msg[0] = '\0';
    xSemaphoreGive(job->mu);
    job->state = JOB_RUNNING;
    if (stack_bytes >= OOBE_STACK_INSTALL && s_hostq != NULL && !s_host_busy) {
        oobe_job_t *jp = job;
        s_host_busy = 1;
        if (xQueueSend(s_hostq, &jp, 0) == pdTRUE) {
            return 0;
        }
        s_host_busy = 0;
    }
    BaseType_t ok = xTaskCreate(worker, "oobe-job", stack_bytes, job, 3, NULL);
    if (ok != pdPASS) {
        vTaskDelay(pdMS_TO_TICKS(30));          /* a finished job's stack is freed by the idle task: give it a moment, then once more */
        ok = xTaskCreate(worker, "oobe-job", stack_bytes, job, 3, NULL);
    }
    if (ok != pdPASS) {
        size_t freeb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        ESP_LOGE("oobe-job", "no room for a %u byte stack: %u bytes of internal RAM free, largest block %u", stack_bytes,
                 (unsigned)freeb, (unsigned)big);
        /* Reported through the job's own result, so every page shows it where it already shows a failure. */
        snprintf(job->msg, sizeof(job->msg), "Not enough memory to do that (%u KB free). Restart and try again.", (unsigned)(big / 1024));
        job->state = JOB_FAILED;
    }
    return 0;
}

int oobe_job_start(oobe_job_t *job, oobe_job_fn fn, void *arg)
{
    return oobe_job_start_stack(job, fn, arg, OOBE_STACK_INSTALL);
}

int oobe_job_running(const oobe_job_t *job) { return job != NULL && job->state == JOB_RUNNING; }

int oobe_job_poll(oobe_job_t *job, char *msg, size_t cap, int *per_mille)
{
    if (job == NULL) {
        return 1;
    }
    xSemaphoreTake(job->mu, portMAX_DELAY);
    if (msg != NULL && cap > 0) {
        snprintf(msg, cap, "%s", job->msg);
    }
    xSemaphoreGive(job->mu);
    if (per_mille != NULL) {
        *per_mille = job->per_mille;
    }
    switch (job->state) {
    case JOB_OK:     return 0;
    case JOB_FAILED: return 1;
    default:         return -1;
    }
}

void oobe_job_report(oobe_job_t *job, const char *msg, int per_mille)
{
    if (job == NULL) {
        return;
    }
    if (msg != NULL) {
        xSemaphoreTake(job->mu, portMAX_DELAY);
        snprintf(job->msg, sizeof(job->msg), "%s", msg);
        xSemaphoreGive(job->mu);
    }
    if (per_mille >= 0) {
        job->per_mille = per_mille > 1000 ? 1000 : per_mille;
    }
}

void oobe_job_abort(oobe_job_t *job, const char *msg)
{
    oobe_job_report(job, msg, -1);
    job->state = JOB_FAILED;
    if (xTaskGetCurrentTaskHandle() == s_host) {
        longjmp(s_host_jb, 1);                     /* running on the main task: back to the host loop, never delete the main task */
    }
    vTaskDelete(NULL);
    for (;;) {                                     /* vTaskDelete(NULL) does not return */
    }
}
