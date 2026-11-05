#include "purr_fetch.h"

#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "fetch";

#define MAX_REDIRECTS 5

static purr_fetch_progress_fn s_progress;
static char s_last_error[96];                    /* "<file>: <why>" of the last fetch that failed; empty after one that worked */

const char *purr_fetch_last_error(void) { return s_last_error; }

void purr_fetch_set_progress(purr_fetch_progress_fn fn) { s_progress = fn; }

/* GitHub (and most release hosting) answers a release asset URL with a redirect to the
 * real object store. esp_http_client only follows redirects on its own inside
 * esp_http_client_perform(); this uses the lower-level open/fetch_headers/read instead
 * (to stream into our own buffer), so redirects are followed by hand here. */
static esp_err_t open_get_ex(const char *url, esp_http_client_handle_t *out, int64_t *content_length,
                             int follow, int timeout_ms)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = timeout_ms,
        /* GitHub's release redirect (to a signed, long-URL S3 object) carries header lines
         * well past a couple of KB; too small a buffer here fails with "Out of buffer". */
        .buffer_size = 8192,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    for (int hop = 0; ; hop++) {
        esp_err_t e = esp_http_client_open(client, 0);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "%s: could not open (%s)", url, esp_err_to_name(e));
            esp_http_client_cleanup(client);
            return e;
        }
        int64_t len = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);

        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            esp_http_client_close(client);
            if (!follow) {
                ESP_LOGW(TAG, "%s: redirect refused", url);
                esp_http_client_cleanup(client);
                return ESP_ERR_HTTP_MAX_REDIRECT;
            }
            if (hop >= MAX_REDIRECTS || esp_http_client_set_redirection(client) != ESP_OK) {
                ESP_LOGW(TAG, "%s: too many redirects", url);
                esp_http_client_cleanup(client);
                return ESP_ERR_HTTP_MAX_REDIRECT;
            }
            continue;
        }
        if (status != 200) {
            ESP_LOGW(TAG, "%s: HTTP %d", url, status);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        *out = client;
        *content_length = len;                       /* -1 if the server did not say */
        return ESP_OK;
    }
}

static esp_err_t open_get(const char *url, esp_http_client_handle_t *out, int64_t *content_length)
{
    return open_get_ex(url, out, content_length, 1, 20000);
}

static void close_client(esp_http_client_handle_t client)
{
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

static esp_err_t fetch_into_inline(const char *url, uint8_t *buf, size_t cap, size_t *out_len)
{
    esp_http_client_handle_t client;
    int64_t content_length;
    esp_err_t e = open_get(url, &client, &content_length);
    if (e != ESP_OK) {
        return e;
    }
    if (content_length > 0 && (size_t)content_length > cap) {
        ESP_LOGW(TAG, "%s: %lld bytes, more than the %u byte buffer", url,
                 (long long)content_length, (unsigned)cap);
        close_client(client);
        return ESP_ERR_NO_MEM;
    }

    size_t got = 0;
    while (got < cap) {
        int n = esp_http_client_read(client, (char *)buf + got, (int)(cap - got));
        if (n < 0) {
            close_client(client);
            return ESP_FAIL;
        }
        if (n == 0) {
            break;                                  /* the body is done */
        }
        got += (size_t)n;
        if (s_progress != NULL) {
            s_progress(got, content_length > 0 ? (size_t)content_length : 0);
        }
    }
    close_client(client);
    *out_len = got;
    return ESP_OK;
}

static esp_err_t fetch_local_into_inline(const char *url, uint8_t *buf, size_t cap, size_t *out_len)
{
    esp_http_client_handle_t client;
    int64_t content_length;
    esp_err_t e = open_get_ex(url, &client, &content_length, 0, 8000);
    if (e != ESP_OK) {
        return e;
    }
    if (content_length > 0 && (size_t)content_length > cap) {
        close_client(client);
        return ESP_ERR_NO_MEM;
    }
    size_t got = 0;
    while (got < cap) {
        int n = esp_http_client_read(client, (char *)buf + got, (int)(cap - got));
        if (n < 0) {
            close_client(client);
            return ESP_FAIL;
        }
        if (n == 0) {
            break;
        }
        got += (size_t)n;
        if (s_progress != NULL) {
            s_progress(got, content_length > 0 ? (size_t)content_length : 0);
        }
    }
    close_client(client);
    *out_len = got;
    return ESP_OK;
}

static esp_err_t fetch_alloc_inline(const char *url, uint8_t **out, size_t *out_len, size_t max_len)
{
    *out = NULL;
    esp_http_client_handle_t client;
    int64_t content_length;
    esp_err_t e = open_get(url, &client, &content_length);
    if (e != ESP_OK) {
        return e;
    }

    /* A known, sane content length lets us allocate exactly once. Otherwise grow in chunks,
     * bounded by max_len (the manifest is small; this path is not for large images). */
    size_t cap = (content_length > 0 && (size_t)content_length <= max_len)
                     ? (size_t)content_length + 1 : 4096;
    uint8_t *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = malloc(cap);
    }
    if (buf == NULL) {
        close_client(client);
        return ESP_ERR_NO_MEM;
    }

    size_t got = 0;
    for (;;) {
        if (got == cap) {
            if (cap >= max_len) {
                free(buf);
                close_client(client);
                return ESP_ERR_NO_MEM;
            }
            size_t next = cap * 2 > max_len ? max_len : cap * 2;
            uint8_t *bigger = heap_caps_malloc(next, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (bigger == NULL) {
                bigger = malloc(next);
            }
            if (bigger == NULL) {
                free(buf);
                close_client(client);
                return ESP_ERR_NO_MEM;
            }
            memcpy(bigger, buf, got);
            free(buf);
            buf = bigger;
            cap = next;
        }
        int n = esp_http_client_read(client, (char *)buf + got, (int)(cap - got));
        if (n < 0) {
            free(buf);
            close_client(client);
            return ESP_FAIL;
        }
        if (n == 0) {
            break;
        }
        got += (size_t)n;
        if (s_progress != NULL) {
            s_progress(got, content_length > 0 ? (size_t)content_length : 0);
        }
    }
    close_client(client);
    *out = buf;
    *out_len = got;
    return ESP_OK;
}

/* ---------------------------------------------------------------- on a worker with its stack in PSRAM
 *
 * A TLS download is the deepest call stack in the system (HTTPS, the handshake, the X.509 chain, 16 KB and more: it overflowed a 16 KB main task on the real device).
 * That depth used to be paid for in fast RAM on whichever task asked: 32 KB on main, 14 to 16 KB on a job. Nothing in a fetch touches flash (it fills a buffer; the
 * install that follows writes it), so the work can run on a worker whose stack is in PSRAM, which has room to spare, and the caller waits on a small stack of its own.
 * A task with a PSRAM stack must never call a flash function itself: keep it that way, and keep the progress callbacks to bookkeeping. If PSRAM has no room for the
 * stack, the fetch runs on the caller's stack as it always did. */

#define WORKER_STACK_BYTES (32 * 1024)

typedef enum { OP_INTO, OP_LOCAL, OP_ALLOC } fetch_op_t;

typedef struct {
    fetch_op_t op;
    const char *url;
    uint8_t *buf;
    size_t cap;                     /* OP_INTO, OP_LOCAL: the buffer's size; OP_ALLOC: max_len */
    uint8_t **out;                  /* OP_ALLOC */
    size_t *out_len;
    esp_err_t rc;
    SemaphoreHandle_t done;
} fetch_req_t;

static esp_err_t fetch_run(fetch_req_t *r)
{
    switch (r->op) {
    case OP_INTO:  return fetch_into_inline(r->url, r->buf, r->cap, r->out_len);
    case OP_LOCAL: return fetch_local_into_inline(r->url, r->buf, r->cap, r->out_len);
    default:       return fetch_alloc_inline(r->url, r->out, r->out_len, r->cap);
    }
}

static void fetch_worker(void *arg)
{
    fetch_req_t *r = arg;
    r->rc = fetch_run(r);
    xSemaphoreGive(r->done);
    vTaskSuspend(NULL);                 /* the caller deletes this task (and frees its PSRAM stack): a task cannot free its own */
}

static esp_err_t fetch_dispatch(fetch_req_t *r)
{
    r->done = xSemaphoreCreateBinary();
    TaskHandle_t worker = NULL;
    if (r->done == NULL ||
        xTaskCreateWithCaps(fetch_worker, "fetch", WORKER_STACK_BYTES, r, uxTaskPriorityGet(NULL), &worker, MALLOC_CAP_SPIRAM) != pdPASS) {
        if (r->done != NULL) {
            vSemaphoreDelete(r->done);
        }
        ESP_LOGW(TAG, "no PSRAM for the fetch stack: running on the caller's");
        return fetch_run(r);
    }
    xSemaphoreTake(r->done, portMAX_DELAY);
    vTaskDeleteWithCaps(worker);
    vSemaphoreDelete(r->done);
    return r->rc;
}

static esp_err_t fetch_note(fetch_req_t *r)
{
    esp_err_t e = fetch_dispatch(r);
    if (e == ESP_OK) {
        s_last_error[0] = '\0';
    } else {
        const char *name = strrchr(r->url, '/');
        snprintf(s_last_error, sizeof(s_last_error), "%.40s: %s", name != NULL ? name + 1 : r->url, esp_err_to_name(e));
        ESP_LOGW(TAG, "fetch of %s failed: %s", r->url, esp_err_to_name(e));
    }
    return e;
}

esp_err_t purr_fetch_into(const char *url, uint8_t *buf, size_t cap, size_t *out_len)
{
    fetch_req_t r = {.op = OP_INTO, .url = url, .buf = buf, .cap = cap, .out_len = out_len};
    return fetch_note(&r);
}

esp_err_t purr_fetch_local_into(const char *url, uint8_t *buf, size_t cap, size_t *out_len)
{
    fetch_req_t r = {.op = OP_LOCAL, .url = url, .buf = buf, .cap = cap, .out_len = out_len};
    return fetch_note(&r);
}

esp_err_t purr_fetch_alloc(const char *url, uint8_t **out, size_t *out_len, size_t max_len)
{
    *out = NULL;
    fetch_req_t r = {.op = OP_ALLOC, .url = url, .cap = max_len, .out = out, .out_len = out_len};
    return fetch_note(&r);
}
