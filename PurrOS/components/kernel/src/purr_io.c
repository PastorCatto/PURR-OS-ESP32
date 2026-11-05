/* purr_io.c - see purr_io.h. */
#include "purr_io.h"

#include "esp_log.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "io";

typedef enum { IO_READ, IO_WRITE, IO_ERASE } io_op_t;

typedef struct {
    io_op_t op;
    const esp_partition_t *p;
    size_t offset;
    void *buf;                      /* the caller's buffer: may be in PSRAM (the flash driver copes), never on the caller's stack for long */
    size_t size;
    esp_err_t rc;
    SemaphoreHandle_t done;
} io_req_t;

static QueueHandle_t s_q;
static TaskHandle_t s_task;

static void io_task(void *arg)
{
    (void)arg;
    io_req_t *r;
    for (;;) {
        if (xQueueReceive(s_q, &r, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (r->op) {
        case IO_READ:  r->rc = esp_partition_read(r->p, r->offset, r->buf, r->size); break;
        case IO_WRITE: r->rc = esp_partition_write(r->p, r->offset, r->buf, r->size); break;
        default:       r->rc = esp_partition_erase_range(r->p, r->offset, r->size); break;
        }
        xSemaphoreGive(r->done);
    }
}

void purr_io_start(void)
{
    if (s_task != NULL) {
        return;
    }
    s_q = xQueueCreate(8, sizeof(io_req_t *));
    if (s_q == NULL || xTaskCreate(io_task, "flash_io", 3072, NULL, 5, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "the flash task did not start: tasks with PSRAM stacks cannot use the file system");
        s_task = NULL;
    }
}

/* The caller's stack is in PSRAM, so it cannot run a flash operation itself. */
static int must_hand_over(void)
{
    int marker;
    return s_task != NULL && !esp_ptr_internal(&marker) && xTaskGetCurrentTaskHandle() != s_task;
}

static esp_err_t hand_over(io_op_t op, const esp_partition_t *p, size_t offset, void *buf, size_t size)
{
    io_req_t r = {.op = op, .p = p, .offset = offset, .buf = buf, .size = size, .rc = ESP_FAIL};
    r.done = xSemaphoreCreateBinary();
    if (r.done == NULL) {
        return ESP_ERR_NO_MEM;
    }
    io_req_t *rp = &r;
    if (xQueueSend(s_q, &rp, portMAX_DELAY) != pdTRUE) {
        vSemaphoreDelete(r.done);
        return ESP_FAIL;
    }
    xSemaphoreTake(r.done, portMAX_DELAY);              /* r lives on this stack until the flash task is done with it */
    vSemaphoreDelete(r.done);
    return r.rc;
}

esp_err_t purr_io_partition_read(const esp_partition_t *p, size_t offset, void *dst, size_t size)
{
    return must_hand_over() ? hand_over(IO_READ, p, offset, dst, size) : esp_partition_read(p, offset, dst, size);
}

esp_err_t purr_io_partition_write(const esp_partition_t *p, size_t offset, const void *src, size_t size)
{
    return must_hand_over() ? hand_over(IO_WRITE, p, offset, (void *)src, size) : esp_partition_write(p, offset, src, size);
}

esp_err_t purr_io_partition_erase_range(const esp_partition_t *p, size_t offset, size_t size)
{
    return must_hand_over() ? hand_over(IO_ERASE, p, offset, NULL, size) : esp_partition_erase_range(p, offset, size);
}
