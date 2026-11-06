/*
 * purr_catrt.c - the device side of the app runtime (purr_catrt.h, plan6.md).
 *
 * Threading: one manager task owns the purr_rt_t and runs every request against it, one at a
 * time. Each app has its own task that runs its callbacks. The manager waits for a callback to
 * return (with a timeout); an app never waits for the manager, so the two cannot deadlock.
 */
#include "esp_attr.h"
#include "purr_catrt.h"
#include "purr_battery.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "sdkconfig.h"

#include "purr_catcalls.h"
#include "purr_settings.h"
#include "purr_runtime.h"

static const char *TAG = "rt";

/* Apps get their task stack, queue and semaphores in PSRAM unless `apps.psram_stacks` is 0 (a way back if an app misbehaves): the file system's raw flash access
 * is handed to the flash task (purr_io.h), so nothing of an app calls flash on its own stack. */
static int use_ext_stacks(void)
{
    return purr_settings_kv_int("apps.psram_stacks", 1) != 0;
}

#define APP_QUEUE_DEPTH   8
#define APP_STACK_BYTES   8192             /* a file listing or a copy runs littlefs on the app's own stack: 6144 was tight */
#define CALL_TIMEOUT_MS   3000
#define APP_INTERNAL_EXTRA 2048            /* its queue, task record and semaphores, on top of the stack */
#define GAME_INTERNAL_RESERVE (8 * 1024)  /* a game in front pauses the shell: only the system's own tasks need internal RAM then */
#define INTERNAL_RESERVE  (16 * 1024)      /* internal RAM kept for the system's own tasks and the 12-14 KB job tasks (Device Manager, Store); 24 KB until 2026-11-05, when
                                            * the apps' stacks moved to PSRAM (an app at 29 KB free could not open: it needed 8 + 2 + 24) */
#define APP_STACK_EXT_BYTES 12288          /* an app's task stack in PSRAM: room for the file system on its own stack, and nothing of it is fast RAM */
#define APP_INTERNAL_EXT    1024           /* what such an app still holds in fast RAM: its task record, and slack */
#define MAX_TIMERS        4
#define EVENT_RING        16
#define REPLY_LEN         1536
#define CONF_PATH         "/etc/apps.conf"
#define CONF_MAX          4096

/* ------------------------------------------------------------------ app slots */

typedef struct {
    purr_rt_event_t ev;
    purr_app_event_t aev;
    char name[32];
    char data[48];
    uint8_t has_aev, has_name, has_data, wait, stop, freeze;
} app_msg_t;

typedef struct {
    uint8_t in_use;
    uint8_t ext_stack;            /* its task, queue and semaphores were made in PSRAM (setting apps.psram_stacks): they are deleted the same way */
    uint8_t task_in_fast;         /* ext_stack, but the task itself could not get a PSRAM stack and was made the old way */
    volatile uint8_t frozen;      /* the runtime asked it to park (see app_task): idle, owns no lock, thawed before it is told anything */
    SemaphoreHandle_t thaw;       /* what a parked task waits on */
    volatile uint8_t closing;     /* teardown has begun: the app's catcalls refuse, so it cannot call into a window that is being deleted */
    int id;                       /* the manager's instance id, set at CREATE */
    TaskHandle_t task;
    QueueHandle_t q;
    SemaphoreHandle_t done;
    void *handle;                 /* the platform's, for unload */
    purr_app_table2_t t2;         /* a copy of the table (a v1 table is widened into this) */
    purr_rt_app_t app;
    char name[PURR_RT_NAME_LEN];
    volatile int result;
    TimerHandle_t timer[MAX_TIMERS];
    char line[96];
    int linelen;
} rtdev_t;

static rtdev_t s_dev[PURR_RT_MAX_APPS];

/* Guards the lifetime of each slot's queue. The UI task, timers and the power/permission glue post to an app's queue from other
 * tasks; teardown deletes it. Without this a second Back tap (or a late permission answer) could send into a queue being freed. */
static StaticSemaphore_t s_qmu_buf;
static SemaphoreHandle_t s_qmu;

/* Queue one message for the app without ever blocking. 0 if queued; -1 if the app is closing, gone, or its queue is full. */
static int q_post(rtdev_t *d, const app_msg_t *m)
{
    int rc = -1;
    if (s_qmu == NULL) {
        return -1;
    }
    xSemaphoreTake(s_qmu, portMAX_DELAY);
    if (d->in_use && !d->closing && d->q != NULL) {
        rc = xQueueSend(d->q, m, 0) == pdTRUE ? 0 : -1;
    }
    xSemaphoreGive(s_qmu);
    return rc;
}
static EXT_RAM_BSS_ATTR purr_rt_t s_rt;
static const void *s_files_table;
static const void *s_devices_table;
static const void *s_store_table;
static const void *s_ui_table, *s_storage_table;       /* registered by purr_catrt_ui.c / purr_catrt_storage.c */
static purr_catrt_ui_lifecycle_fn s_ui_lifecycle;
static void (*s_storage_release)(int slot);
static const void *s_power_table;
static void (*s_power_stopped)(int slot, const char *name);
static void (*s_power_paused)(int slot);
static void (*s_power_frozen)(int slot);
static const void *s_game_table;
static void (*s_game_lifecycle)(int slot, int ev);
static portMUX_TYPE s_notice_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_notice[64];
static const purr_catrt_platform_t *s_plat;

/* The event ring: one writer (the manager task), any number of polling readers. */
static char s_ring[EVENT_RING][PURR_RT_LINE_LEN * 2];
static volatile uint32_t s_seq;

static void notice_set(const char *fmt, const char *name)
{
    char tmp[sizeof(s_notice)];
    snprintf(tmp, sizeof(tmp), fmt, name);
    portENTER_CRITICAL(&s_notice_mux);
    memcpy(s_notice, tmp, sizeof(s_notice));
    portEXIT_CRITICAL(&s_notice_mux);
}

int purr_catrt_take_notice(char *buf, size_t cap)
{
    int got = 0;
    portENTER_CRITICAL(&s_notice_mux);
    if (s_notice[0] != '\0' && cap > 0) {
        snprintf(buf, cap, "%s", s_notice);
        s_notice[0] = '\0';
        got = 1;
    }
    portEXIT_CRITICAL(&s_notice_mux);
    return got;
}

static void sink_cb(void *ctx, const char *line)
{
    (void)ctx;
    ESP_LOGI(TAG, "%s", line);
    if (strncmp(line, "evicted>>", 9) == 0) {            /* "evicted>>3,notes" */
        const char *name = strchr(line + 9, ',');
        if (name != NULL) {
            char n[24];
            snprintf(n, sizeof(n), "%s", name + 1);
            notice_set("Closed %.24s to free memory", n);
        }
    } else if (strncmp(line, "refused>>", 9) == 0 && strstr(line, ",service-slots") != NULL) {
        char n[24];
        snprintf(n, sizeof(n), "%s", line + 9);
        char *comma = strchr(n, ',');
        if (comma != NULL) {
            *comma = '\0';
        }
        notice_set("%.24s cannot run in the background: no free slot", n);
    }
    snprintf(s_ring[s_seq % EVENT_RING], sizeof(s_ring[0]), "%s", line);
    s_seq++;
}

uint32_t purr_catrt_event_cursor(void)
{
    return s_seq;
}

int purr_catrt_event_next(uint32_t *cursor, char *line, size_t cap)
{
    uint32_t seq = s_seq;
    if (*cursor == seq) {
        return 0;
    }
    if (seq - *cursor > EVENT_RING) {
        *cursor = seq - EVENT_RING;            /* a slow reader skips ahead */
    }
    snprintf(line, cap, "%s", s_ring[*cursor % EVENT_RING]);
    (*cursor)++;
    return 1;
}

static rtdev_t *cur_dev(void)
{
    TaskHandle_t t = xTaskGetCurrentTaskHandle();
    for (int i = 0; i < PURR_RT_MAX_APPS; i++) {
        if (s_dev[i].in_use && s_dev[i].task == t) {
            return &s_dev[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ requests to the manager */

typedef enum {
    R_DISPATCH, R_QUERY, R_KEY, R_RELOAD, R_OPEN_BOOT, R_OPEN_EXEC, R_EMIT, R_POST, R_SUB, R_UNSUB, R_CLOSE,
    R_FOCUS,
} req_kind_t;

typedef struct {
    req_kind_t kind;
    char line[PURR_RT_LINE_LEN];
    char arg[48];
    int id;
    unsigned mods;
    char out[REPLY_LEN];
    int rc;
    SemaphoreHandle_t done;       /* NULL for a request nobody waits for */
    int refs;                     /* the caller and the manager each hold one; the last frees it */
} req_t;

static QueueHandle_t s_reqq;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void req_release(req_t *r)
{
    portENTER_CRITICAL(&s_mux);
    int n = --r->refs;
    portEXIT_CRITICAL(&s_mux);
    if (n == 0) {
        if (r->done) {
            vSemaphoreDelete(r->done);
        }
        free(r);
    }
}

static req_t *req_post(req_kind_t kind, const char *line, const char *arg, int id, unsigned mods,
                       int wait)
{
    if (s_reqq == NULL) {
        return NULL;
    }
    req_t *r = calloc(1, sizeof(*r));
    if (r == NULL) {
        return NULL;
    }
    r->kind = kind;
    r->id = id;
    r->mods = mods;
    if (line) {
        snprintf(r->line, sizeof(r->line), "%s", line);
    }
    if (arg) {
        snprintf(r->arg, sizeof(r->arg), "%s", arg);
    }
    r->refs = wait ? 2 : 1;
    if (wait) {
        r->done = xSemaphoreCreateBinary();
        if (r->done == NULL) {
            free(r);
            return NULL;
        }
    }
    if (xQueueSend(s_reqq, &r, pdMS_TO_TICKS(500)) != pdTRUE) {
        if (r->done) {
            vSemaphoreDelete(r->done);
        }
        free(r);
        return NULL;
    }
    return r;
}

/* Waits for a request posted with wait = 1. On success copies the reply text out. */
static int req_wait(req_t *r, int timeout_ms, char *out, size_t cap)
{
    int ok = xSemaphoreTake(r->done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    int rc = -1;
    if (ok) {
        rc = r->rc;
        if (out && cap) {
            snprintf(out, cap, "%s", r->out);
        }
    } else if (out && cap) {
        snprintf(out, cap, "error: the app manager did not answer\n");
    }
    req_release(r);
    return rc;
}

static int rpc(req_kind_t kind, const char *line, const char *arg, int id, unsigned mods, char *out,
               size_t cap)
{
    req_t *r = req_post(kind, line, arg, id, mods, 1);
    if (r == NULL) {
        if (out && cap) {
            snprintf(out, cap, "error: the app manager is not running\n");
        }
        return -1;
    }
    return req_wait(r, 8000, out, cap);
}

static void post_async(req_kind_t kind, const char *line, const char *arg, int id)
{
    (void)req_post(kind, line, arg, id, 0, 0);
}

int purr_catrt_dispatch(const char *line, char *out, size_t cap)
{
    return rpc(R_DISPATCH, line, NULL, 0, 0, out, cap);
}

int purr_catrt_query(const char *line, char *out, size_t cap)
{
    return rpc(R_QUERY, line, NULL, 0, 0, out, cap);
}

int purr_catrt_key(unsigned mods, const char *key)
{
    return rpc(R_KEY, key, NULL, 0, mods, NULL, 0);
}

int purr_catrt_reload(char *err, size_t errcap)
{
    return rpc(R_RELOAD, NULL, NULL, 0, 0, err, errcap);
}

/* ------------------------------------------------------------------ the default config */

static const char s_default_conf[] =
    "# /etc/apps.conf - how the app runtime behaves. Hyprland-style: keyword = value.\n"
    "#\n"
    "# general:max_apps = 8\n"
    "# general:reserve_kb = 64       # memory kept free for the system when admitting an app\n"
    "# general:low_kb = 512          # below this free memory, apps are told to trim\n"
    "# general:critical_kb = 256     # below this, the least important apps are stopped\n"
    "#\n"
    "# rule = background, name:sync   # let an app keep running when it leaves the front\n"
    "# rule = persistent, class:daemon\n"
    "# bind = SUPER, Tab, cycle\n"
    "#\n"
    "# Autostart. exec-boot apps are the ones the system needs with nobody logged in; exec-once apps\n"
    "# start after the first login of a boot, and not before.\n"
    "# exec-boot = sync\n"
    "# exec-once = notes\n";

/* ------------------------------------------------------------------ the manager's host */

static uint32_t host_now(void *ctx)
{
    (void)ctx;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint32_t host_free(void *ctx)
{
    (void)ctx;
    return (uint32_t)esp_get_free_heap_size();
}

/* An app's task stack comes from INTERNAL RAM (it cannot live in PSRAM: apps write flash), which is a small part of what host_free()
 * counts. This is the number admission and the memory-emergency check use. */
static uint32_t host_internal_free(void *ctx, uint32_t *largest)
{
    (void)ctx;
    *largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void host_log(void *ctx, const char *line)
{
    (void)ctx;
    ESP_LOGI(TAG, "%s", line);
}

static int host_save_state(void *ctx, const char *name, const void *buf, uint32_t len)
{
    (void)ctx;
    return s_plat->save_state ? s_plat->save_state(name, buf, len) : -1;
}

static int host_load_state(void *ctx, const char *name, void *buf, uint32_t cap)
{
    (void)ctx;
    return s_plat->load_state ? s_plat->load_state(name, buf, cap) : 0;
}

/* Calls the app makes before it has a console of its own: the shared environment. */
static void cc_print(const char *s);
static const void *catcall_get(const char *name, uint32_t min_version);

static const purr_app_env_t s_env = {
    .console_print = cc_print,
    .env_version = PURR_APP_ABI_VERSION_2,
    .catcall_get = catcall_get,
};

static int host_load(void *ctx, const char *name, purr_rt_app_t *out)
{
    (void)ctx;
    rtdev_t *d = NULL;
    for (int i = 0; i < PURR_RT_MAX_APPS; i++) {
        if (!s_dev[i].in_use) {
            d = &s_dev[i];
            break;
        }
    }
    if (d == NULL) {
        return -1;
    }
    const purr_app_table_t *t1 = NULL;
    void *handle = NULL;
    int lr = s_plat->load ? s_plat->load(name, &t1, &handle) : -1;
    if (lr != 0 || t1 == NULL) {
        return lr != 0 ? lr : -1;               /* PURR_RT_HOST_UNTRUSTED passes through */
    }
    memset(d, 0, sizeof(*d));
    if (t1->abi_version == PURR_APP_ABI_VERSION_2) {
        d->t2 = *(const purr_app_table2_t *)t1;
    } else if (t1->abi_version == PURR_APP_ABI_VERSION) {
        /* A v1 app: create and destroy only, run to completion on its own task. */
        d->t2.base = *t1;
        d->t2.kind = PURR_APP_KIND_COMMAND_LINE;
        d->t2.memory_needed = 16384;
    } else {
        if (s_plat->unload) {
            s_plat->unload(handle);
        }
        return -1;
    }
    d->in_use = 1;
    d->handle = handle;
    snprintf(d->name, sizeof(d->name), "%s", name);

    memset(out, 0, sizeof(*out));
    out->table = &d->t2;
    out->env = &s_env;
    out->handle = (uint32_t)(d - s_dev) + 1;
    int ext = use_ext_stacks();
    out->memory_needed = (d->t2.memory_needed ? d->t2.memory_needed : 16384) + (ext ? APP_STACK_EXT_BYTES : APP_STACK_BYTES);
    out->internal_needed = ext ? APP_INTERNAL_EXT : APP_STACK_BYTES + APP_INTERNAL_EXTRA;
    out->kind = d->t2.kind;
    out->flags = d->t2.flags;
    return 0;
}

static void teardown(rtdev_t *d)
{
    d->closing = 1;
    if (d->frozen && d->thaw) {                 /* a parked task cannot read its stop message */
        d->frozen = 0;
        xSemaphoreGive(d->thaw);
    }
    if (s_power_stopped != NULL) {
        s_power_stopped((int)(d - s_dev), d->name);       /* its locks, alarms and "just once" grants go with it */
    }
    if (s_storage_release != NULL) {
        s_storage_release((int)(d - s_dev));        /* flush and close what the app left open */
    }
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (d->timer[i]) {
            xTimerStop(d->timer[i], 0);
            xTimerDelete(d->timer[i], 0);
            d->timer[i] = NULL;
        }
    }
    if (d->task) {
        app_msg_t m;
        memset(&m, 0, sizeof(m));
        m.stop = 1;
        xQueueSend(d->q, &m, pdMS_TO_TICKS(100));       /* d->closing is set: nobody else posts, so this is the last message */
        for (int i = 0; i < 200 && d->task; i++) {      /* it was told to destroy and answered, so this is normally instant; a forced delete */
            vTaskDelay(pdMS_TO_TICKS(10));              /* can leave a file system or UI lock held, so give a straggler 2 s */
        }
        if (d->task) {                          /* stuck in a callback: force it (SPEC section 9) */
            if (d->ext_stack && !d->task_in_fast) {
                vTaskDeleteWithCaps(d->task);
            } else {
                vTaskDelete(d->task);
            }
            d->task = NULL;
        }
    }
    if (s_game_lifecycle != NULL && d->t2.kind == PURR_APP_KIND_GAME) {
        s_game_lifecycle((int)(d - s_dev), PURR_RT_EV_DESTROY);             /* the screen goes back to the shell */
    }
    if (s_ui_lifecycle != NULL && d->t2.kind == PURR_APP_KIND_UI) {      /* only now that the app can no longer call into it */
        s_ui_lifecycle((int)(d - s_dev), d->name, PURR_RT_EV_DESTROY);   /* its window and widgets go with it */
    }
    QueueHandle_t q = d->q;
    xSemaphoreTake(s_qmu, portMAX_DELAY);               /* a poster that got past the closing check is finished before this returns */
    d->q = NULL;
    xSemaphoreGive(s_qmu);
    if (q) {
        if (d->ext_stack) {
            vQueueDeleteWithCaps(q);
        } else {
            vQueueDelete(q);
        }
    }
    if (d->done) {
        if (d->ext_stack) {
            vSemaphoreDeleteWithCaps(d->done);
        } else {
            vSemaphoreDelete(d->done);
        }
    }
    if (d->thaw) {
        if (d->ext_stack) {
            vSemaphoreDeleteWithCaps(d->thaw);
        } else {
            vSemaphoreDelete(d->thaw);
        }
    }
    if (s_plat->unload) {
        s_plat->unload(d->handle);
    }
    memset(d, 0, sizeof(*d));
}

/* The runtime freezes an app that left the screen and thaws it before it is told anything again. The task is never suspended from the
 * outside (it could be inside littlefs or waiting on the UI task, holding their locks): a "freeze" message goes into its queue, behind
 * whatever it is still doing, and the task parks itself when it reaches it. Its power locks and alarms go when it freezes: a frozen app
 * keeps nothing awake. */
static void host_freeze(void *ctx, uint32_t handle, int frozen)
{
    (void)ctx;
    if (handle == 0 || handle > PURR_RT_MAX_APPS) {
        return;
    }
    rtdev_t *d = &s_dev[handle - 1];
    if (!d->in_use || d->task == NULL || d->closing) {
        return;
    }
    if (frozen && !d->frozen) {
        app_msg_t m;
        memset(&m, 0, sizeof(m));
        m.freeze = 1;
        d->frozen = 1;
        if (xQueueSend(d->q, &m, pdMS_TO_TICKS(100)) != pdTRUE) {
            d->frozen = 0;                                  /* the queue is full: leave it running rather than lose track */
            return;
        }
        if (s_power_frozen != NULL) {
            s_power_frozen((int)(d - s_dev));
        }
    } else if (!frozen && d->frozen) {
        d->frozen = 0;
        xSemaphoreGive(d->thaw);
    }
}

static void host_unload(void *ctx, uint32_t handle)
{
    (void)ctx;
    if (handle == 0 || handle > PURR_RT_MAX_APPS) {
        return;
    }
    rtdev_t *d = &s_dev[handle - 1];
    if (d->in_use) {
        teardown(d);
    }
}

/* Each app's own task: it runs the callbacks the manager (or a timer) queues for it. */
static void app_task(void *arg)
{
    rtdev_t *d = arg;
    app_msg_t m;
    for (;;) {
        if (xQueueReceive(d->q, &m, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (m.stop) {
            break;
        }
        if (m.freeze) {
            /* Parked between two messages, so it holds no lock and has no call in flight. host_freeze() asked for this; if the app was
             * thawed before this message came up there is nothing to wait for (and a give that raced ahead is cleared). */
            if (d->frozen) {
                xSemaphoreTake(d->thaw, portMAX_DELAY);
            } else {
                xSemaphoreTake(d->thaw, 0);
            }
            continue;
        }
        if (m.has_aev) {
            m.aev.name = m.has_name ? m.name : NULL;
            m.aev.data = m.has_data ? m.data : NULL;
            m.ev.app_ev = &m.aev;
        }
        int r = purr_rt_invoke(&d->app, &m.ev);
        if (m.wait) {
            d->result = r;
            xSemaphoreGive(d->done);
        }
    }
    d->task = NULL;
    if (d->ext_stack && !d->task_in_fast) {
        vTaskDeleteWithCaps(NULL);                      /* made with a PSRAM stack: IDF frees that stack, and the task record */
    } else {
        vTaskDelete(NULL);
    }
}

static int host_deliver(void *ctx, int id, const purr_rt_app_t *app, const purr_rt_event_t *ev)
{
    (void)ctx;
    rtdev_t *d = &s_dev[app->handle - 1];
    if (ev->type == PURR_RT_EV_CREATE) {
        d->id = id;
        d->app = *app;
        d->ext_stack = (uint8_t)use_ext_stacks();
        d->task_in_fast = 0;
        if (d->ext_stack) {
            d->q = xQueueCreateWithCaps(APP_QUEUE_DEPTH, sizeof(app_msg_t), MALLOC_CAP_SPIRAM);
            d->done = xSemaphoreCreateBinaryWithCaps(MALLOC_CAP_SPIRAM);
            d->thaw = xSemaphoreCreateBinaryWithCaps(MALLOC_CAP_SPIRAM);
        } else {
            d->q = xQueueCreate(APP_QUEUE_DEPTH, sizeof(app_msg_t));
            d->done = xSemaphoreCreateBinary();
            d->thaw = xSemaphoreCreateBinary();
        }
        char tname[configMAX_TASK_NAME_LEN];
        snprintf(tname, sizeof(tname), "app:%.11s", d->name);
        BaseType_t made = pdFAIL;
        if (d->q != NULL && d->done != NULL && d->thaw != NULL) {
            if (d->ext_stack) {
                made = xTaskCreateWithCaps(app_task, tname, APP_STACK_EXT_BYTES, d, 4, &d->task, MALLOC_CAP_SPIRAM);
            }
            if (made != pdPASS) {                       /* no PSRAM stack (or switched off): the old way, 8 KB of fast RAM; the queue and semaphores are deleted as they were made */
                made = xTaskCreate(app_task, tname, APP_STACK_BYTES, d, 4, &d->task);
                if (made == pdPASS && d->ext_stack) {
                    ESP_LOGW(TAG, "%s: no PSRAM for its stack, running on a fast RAM one", d->name);
                    d->task_in_fast = 1;
                }
            }
        }
        if (made != pdPASS) {
            ESP_LOGE(TAG, "%s: could not start its task", d->name);
            return -1;
        }
    }
    if (d->task == NULL) {
        return -1;
    }
    app_msg_t m;
    memset(&m, 0, sizeof(m));
    m.ev = *ev;
    m.wait = 1;
    if (ev->app_ev) {
        m.has_aev = 1;
        m.aev = *ev->app_ev;
        if (ev->app_ev->name) {
            m.has_name = 1;
            snprintf(m.name, sizeof(m.name), "%s", ev->app_ev->name);
        }
        if (ev->app_ev->data) {
            m.has_data = 1;
            snprintf(m.data, sizeof(m.data), "%s", ev->app_ev->data);
        }
    }
    xSemaphoreTake(d->done, 0);                 /* drop a stale give from an earlier timeout */
    if (xQueueSend(d->q, &m, pdMS_TO_TICKS(200)) != pdTRUE) {
        return -1;
    }
    if (xSemaphoreTake(d->done, pdMS_TO_TICKS(CALL_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "%s did not answer within %d ms", d->name, CALL_TIMEOUT_MS);
        char data[PURR_RT_LINE_LEN];
        snprintf(data, sizeof(data), "%d,%s", id, d->name);
        purr_rt_emit(&s_rt, "unresponsive", data);
        return -1;
    }
    int rr = d->result;
    if (s_ui_lifecycle != NULL && d->t2.kind == PURR_APP_KIND_UI &&
        (ev->type == PURR_RT_EV_RESUME || ev->type == PURR_RT_EV_PAUSE)) {
        s_ui_lifecycle((int)(d - s_dev), d->name, ev->type);              /* show or hide its window */
    }
    if (s_game_lifecycle != NULL && d->t2.kind == PURR_APP_KIND_GAME &&
        (ev->type == PURR_RT_EV_RESUME || ev->type == PURR_RT_EV_PAUSE)) {
        s_game_lifecycle((int)(d - s_dev), ev->type);                      /* the screen and the buttons change hands */
    }
    if (ev->type == PURR_RT_EV_PAUSE && s_power_paused != NULL) {
        s_power_paused((int)(d - s_dev));                                  /* a screen lock ends with its time in front */
    }
    return rr;
}

/* ------------------------------------------------------------------ the catcalls */

static void con_flush_line(rtdev_t *d)
{
    d->line[d->linelen] = '\0';
    ESP_LOGI("app", "%s: %s", d->name, d->line);
    d->linelen = 0;
}

static void cc_write(const char *buf, size_t len)
{
    rtdev_t *d = cur_dev();
    if (d == NULL || buf == NULL) {
        return;
    }
    /* The foreground app writes to the terminal; everything an app prints also goes to the
     * system log, a line at a time and tagged with its name. */
    if (s_rt.fg_id == d->id && s_plat->console_write) {
        s_plat->console_write(buf, len);
    }
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            con_flush_line(d);
        } else if (d->linelen < (int)sizeof(d->line) - 1) {
            d->line[d->linelen++] = buf[i];
        }
    }
}

static void cc_print(const char *s)
{
    if (s) {
        cc_write(s, strlen(s));
    }
}

static int cc_read_line(char *buf, size_t cap, int timeout_ms)
{
    (void)timeout_ms;                           /* console input for apps comes with the shell hookup */
    if (cap) {
        buf[0] = '\0';
    }
    return 0;
}

static const purr_cc_console_t s_cc_console = {
    .version = 1, .write = cc_write, .print = cc_print, .read_line = cc_read_line,
};

static int cc_app_info(purr_app_info_t *out)
{
    rtdev_t *d = cur_dev();
    if (d == NULL || out == NULL) {
        return -1;
    }
    const purr_rt_inst_t *in = purr_rt_get(&s_rt, d->id);
    if (in == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->id = in->id;
    snprintf(out->name, sizeof(out->name), "%s", in->name);
    out->state = (uint8_t)in->state;
    out->kind = in->app.kind;
    out->flags = in->flags;
    out->privileged = (in->flags & PURR_APP_F_PRIVILEGED) != 0;
    out->memory = in->memory;
    out->uptime_ms = host_now(NULL) - in->started_ms;
    return 0;
}

static void cc_close_self(void)
{
    rtdev_t *d = cur_dev();
    if (d) {
        post_async(R_CLOSE, NULL, NULL, d->id);
    }
}

static int cc_request_focus(void)
{
    rtdev_t *d = cur_dev();
    if (d == NULL) {
        return -1;
    }
    post_async(R_FOCUS, NULL, NULL, d->id);
    return 0;
}

static int cc_post_self(const char *name, const char *data)
{
    rtdev_t *d = cur_dev();
    if (d == NULL || name == NULL) {
        return -1;
    }
    post_async(R_POST, name, data, d->id);
    return 0;
}

static const purr_cc_app_t s_cc_app = {
    .version = 1, .info = cc_app_info, .close_self = cc_close_self,
    .request_focus = cc_request_focus, .post_self = cc_post_self,
};

static uint32_t cc_uptime_ms(void)
{
    return host_now(NULL);
}

static void cc_sleep_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void timer_cb(TimerHandle_t t)
{
    uintptr_t code = (uintptr_t)pvTimerGetTimerID(t);
    rtdev_t *d = &s_dev[code >> 8];
    app_msg_t m;
    memset(&m, 0, sizeof(m));
    m.has_aev = 1;
    m.aev.type = PURR_APPEV_TIMER;
    m.aev.arg = (int32_t)(code & 0xff) + 1;
    m.ev.type = PURR_RT_EV_APP;
    q_post(d, &m);                              /* a full queue drops the tick, as it should */
}

static int cc_timer_start(uint32_t ms, int repeat)
{
    rtdev_t *d = cur_dev();
    if (d == NULL) {
        return -1;
    }
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (d->timer[i] == NULL) {
            uintptr_t code = ((uintptr_t)(d - s_dev) << 8) | (uintptr_t)i;
            TimerHandle_t t = xTimerCreate("apptmr", pdMS_TO_TICKS(ms ? ms : 1),
                                           repeat ? pdTRUE : pdFALSE, (void *)code, timer_cb);
            if (t == NULL) {
                return -1;
            }
            d->timer[i] = t;
            xTimerStart(t, 0);
            return i + 1;
        }
    }
    return -1;
}

static void cc_timer_cancel(int id)
{
    rtdev_t *d = cur_dev();
    if (d == NULL || id < 1 || id > MAX_TIMERS || d->timer[id - 1] == NULL) {
        return;
    }
    xTimerStop(d->timer[id - 1], 0);
    xTimerDelete(d->timer[id - 1], 0);
    d->timer[id - 1] = NULL;
}

static const purr_cc_sched_t s_cc_sched = {
    .version = 1, .uptime_ms = cc_uptime_ms, .sleep_ms = cc_sleep_ms,
    .timer_start = cc_timer_start, .timer_cancel = cc_timer_cancel,
};

static uint32_t cc_free_heap(void) { return (uint32_t)esp_get_free_heap_size(); }
static uint32_t cc_largest_block(void) { return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT); }
static int cc_battery(void) { return purr_battery_percent(); }
static int64_t cc_unix_time(void)
{
    time_t t = time(NULL);
    return t < 1000000000 ? 0 : (int64_t)t;     /* an unset clock reads as 0 */
}

/* The time in the device's zone (the TZ the settings applied); < 0 while the clock is unset. */
static int cc_local_time(purr_cc_tm_t *out)
{
    time_t t = time(NULL);
    if (out == NULL || t < 1000000000) {
        return -1;
    }
    struct tm tm;
    localtime_r(&t, &tm);
    out->year = tm.tm_year + 1900;
    out->mon = tm.tm_mon + 1;
    out->mday = tm.tm_mday;
    out->hour = tm.tm_hour;
    out->min = tm.tm_min;
    out->sec = tm.tm_sec;
    out->wday = tm.tm_wday;
    return 0;
}

static const purr_cc_system_t s_cc_system = {
    .version = 2, .uptime_ms = cc_uptime_ms, .free_heap = cc_free_heap,
    .largest_free_block = cc_largest_block, .battery_percent = cc_battery, .unix_time = cc_unix_time, .local_time = cc_local_time,
};

static int cc_emit(const char *name, const char *data)
{
    rtdev_t *d = cur_dev();
    if (d == NULL || name == NULL) {
        return -1;
    }
    char full[PURR_RT_LINE_LEN];
    snprintf(full, sizeof(full), "custom.%s.%s", d->name, name);
    post_async(R_EMIT, full, data ? data : "", d->id);
    return 0;
}

static int cc_subscribe(const char *prefix)
{
    rtdev_t *d = cur_dev();
    if (d == NULL || prefix == NULL) {
        return -1;
    }
    post_async(R_SUB, prefix, NULL, d->id);
    return 0;
}

static void cc_unsubscribe(const char *prefix)
{
    rtdev_t *d = cur_dev();
    if (d && prefix) {
        post_async(R_UNSUB, prefix, NULL, d->id);
    }
}

static const purr_cc_event_t s_cc_event = {
    .version = 1, .emit = cc_emit, .subscribe = cc_subscribe, .unsubscribe = cc_unsubscribe,
};

void purr_catrt_set_ui(const void *table, purr_catrt_ui_lifecycle_fn lifecycle)
{
    s_ui_table = table;
    s_ui_lifecycle = lifecycle;
}

void purr_catrt_set_storage(const void *table, void (*release)(int slot))
{
    s_storage_table = table;
    s_storage_release = release;
}

void purr_catrt_set_files(const void *table)
{
    s_files_table = table;
}

void purr_catrt_set_devices(const void *table)
{
    s_devices_table = table;
}

void purr_catrt_set_store(const void *table)
{
    s_store_table = table;
}

int purr_catrt_caller(char *name, size_t cap)
{
    rtdev_t *d = cur_dev();
    if (d == NULL || d->closing) {                      /* a closing app gets no more windows, files or sockets */
        return -1;
    }
    if (name != NULL && cap > 0) {
        snprintf(name, cap, "%s", d->name);
    }
    return (int)(d - s_dev);
}

void purr_catrt_set_game(const void *table, void (*lifecycle)(int slot, int ev))
{
    s_game_table = table;
    s_game_lifecycle = lifecycle;
}

void purr_catrt_set_power_frozen(void (*frozen)(int slot))
{
    s_power_frozen = frozen;
}

void purr_catrt_set_power(const void *table, void (*stopped)(int slot, const char *name), void (*paused)(int slot))
{
    s_power_table = table;
    s_power_stopped = stopped;
    s_power_paused = paused;
}

int purr_catrt_slot_info(int slot, purr_catrt_slot_info_t *out)
{
    if (slot < 0 || slot >= PURR_RT_MAX_APPS || !s_dev[slot].in_use || out == NULL) {
        return -1;
    }
    rtdev_t *d = &s_dev[slot];
    snprintf(out->name, sizeof(out->name), "%s", d->name);
    out->foreground = d->id != 0 && d->id == s_rt.fg_id;
    uint8_t flags = d->t2.flags;                              /* the header's, unless a rule changed them */
    for (int i = 0; i < PURR_RT_MAX_APPS; i++) {
        if (d->id != 0 && s_rt.inst[i].id == d->id) {
            flags = s_rt.inst[i].flags;
            /* An app still in on_create is about to take the screen: what it asks for there (Device Manager asks for
             * manage-devices at once) may prompt. Counting it as a background app refused it with no prompt ever shown. */
            if (s_rt.inst[i].state == PURR_RT_STARTING && d->t2.kind == PURR_APP_KIND_UI && !(flags & PURR_APP_F_NOFOCUS)) {
                out->foreground = 1;
            }
        }
    }
    out->privileged = (flags & PURR_APP_F_PRIVILEGED) != 0;
    out->background_ok = d->t2.kind == PURR_APP_KIND_DAEMON || (flags & PURR_APP_F_BACKGROUND_OK) != 0;
    out->kind = d->t2.kind;
    return 0;
}

int purr_catrt_tasks(purr_catrt_task_t *out, int max)
{
    int n = 0;
    for (int slot = 0; slot < PURR_RT_MAX_APPS && n < max; slot++) {
        rtdev_t *d = &s_dev[slot];
        if (!d->in_use || d->closing || d->id == 0) {
            continue;
        }
        const purr_rt_inst_t *in = NULL;
        for (int i = 0; i < PURR_RT_MAX_APPS; i++) {
            if (s_rt.inst[i].id == d->id) {
                in = &s_rt.inst[i];
            }
        }
        if (in == NULL || in->state == PURR_RT_STARTING || in->state == PURR_RT_STOPPING || in->state == PURR_RT_STOPPED) {
            continue;
        }
        purr_catrt_task_t *t = &out[n++];
        memset(t, 0, sizeof(*t));
        t->slot = slot;
        t->id = d->id;
        snprintf(t->name, sizeof(t->name), "%s", d->name);
        t->kind = d->t2.kind;
        t->foreground = in->state == PURR_RT_FOREGROUND;
        t->frozen = d->frozen != 0;
        t->service = d->t2.kind == PURR_APP_KIND_DAEMON;
        t->memory = in->memory;
    }
    return n;
}

void purr_catrt_home(void)
{
    post_async(R_DISPATCH, "background active", NULL, 0);
}

void purr_catrt_focus_slot(int slot)
{
    if (slot >= 0 && slot < PURR_RT_MAX_APPS && s_dev[slot].in_use && !s_dev[slot].closing) {
        post_async(R_FOCUS, NULL, NULL, s_dev[slot].id);
    }
}

int purr_catrt_slot_of(const char *name)
{
    for (int i = 0; name != NULL && i < PURR_RT_MAX_APPS; i++) {
        if (s_dev[i].in_use && strcmp(s_dev[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

int purr_catrt_post_event(int slot, int type, int arg, const char *name)
{
    if (slot < 0 || slot >= PURR_RT_MAX_APPS) {
        return -1;
    }
    app_msg_t m;
    memset(&m, 0, sizeof(m));
    m.has_aev = 1;
    m.aev.type = (uint32_t)type;
    m.aev.arg = arg;
    if (name != NULL) {
        m.has_name = 1;
        snprintf(m.name, sizeof(m.name), "%s", name);
    }
    m.ev.type = PURR_RT_EV_APP;
    return q_post(&s_dev[slot], &m);
}

int purr_catrt_post_ui(int slot, int handle, const char *what)
{
    if (slot < 0 || slot >= PURR_RT_MAX_APPS || what == NULL) {
        return -1;
    }
    rtdev_t *d = &s_dev[slot];
    app_msg_t m;
    memset(&m, 0, sizeof(m));
    m.has_aev = 1;
    m.has_name = 1;
    m.aev.type = PURR_APPEV_UI;
    m.aev.arg = handle;
    snprintf(m.name, sizeof(m.name), "%s", what);
    m.ev.type = PURR_RT_EV_APP;
    return q_post(d, &m);                                    /* a full queue drops it, never blocks */
}

void purr_catrt_close_slot(int slot)
{
    if (slot >= 0 && slot < PURR_RT_MAX_APPS && s_dev[slot].in_use) {
        post_async(R_CLOSE, NULL, NULL, s_dev[slot].id);
    }
}

/* `ctl` comes with the permissions chunk; an app that asks for it today is told it is not available, which
 * is what catcall_get is for. `ui` goes only to an app whose kind is ui; `storage` and `ui` only when the
 * file that provides them has registered. */
static const void *catcall_get(const char *name, uint32_t min_version)
{
    if (name == NULL) {
        return NULL;
    }
    if (strcmp(name, "console") == 0 && min_version <= s_cc_console.version) {
        return &s_cc_console;
    }
    if (strcmp(name, "app") == 0 && min_version <= s_cc_app.version) {
        return &s_cc_app;
    }
    if (strcmp(name, "ui") == 0 && s_ui_table != NULL && min_version <= ((const uint32_t *)s_ui_table)[0]) {
        rtdev_t *d = cur_dev();
        return d != NULL && d->t2.kind == PURR_APP_KIND_UI ? s_ui_table : NULL;
    }
    if (strcmp(name, "game") == 0 && s_game_table != NULL && min_version <= ((const uint32_t *)s_game_table)[0]) {
        rtdev_t *d = cur_dev();
        return d != NULL && d->t2.kind == PURR_APP_KIND_GAME ? s_game_table : NULL;
    }
    if (strcmp(name, "power") == 0 && s_power_table != NULL && min_version <= ((const uint32_t *)s_power_table)[0]) {
        return s_power_table;
    }
    if (strcmp(name, "storage") == 0 && s_storage_table != NULL &&
        min_version <= ((const uint32_t *)s_storage_table)[0]) {
        return s_storage_table;
    }
    if (strcmp(name, "store") == 0 && s_store_table != NULL && min_version <= ((const uint32_t *)s_store_table)[0]) {
        return s_store_table;
    }
    if (strcmp(name, "devices") == 0 && s_devices_table != NULL && min_version <= ((const uint32_t *)s_devices_table)[0]) {
        return s_devices_table;
    }
    if (strcmp(name, "files") == 0 && s_files_table != NULL && min_version <= ((const uint32_t *)s_files_table)[0]) {
        return s_files_table;
    }
    if (strcmp(name, "sched") == 0 && min_version <= s_cc_sched.version) {
        return &s_cc_sched;
    }
    if (strcmp(name, "system") == 0 && min_version <= s_cc_system.version) {
        return &s_cc_system;
    }
    if (strcmp(name, "event") == 0 && min_version <= s_cc_event.version) {
        return &s_cc_event;
    }
    return NULL;
}

/* ------------------------------------------------------------------ the manager task */

static int do_reload(req_t *r)
{
    char *text = malloc(CONF_MAX);
    if (text == NULL) {
        snprintf(r->out, sizeof(r->out), "error: out of memory\n");
        return -1;
    }
    int n = s_plat->read_file ? s_plat->read_file(CONF_PATH, text, CONF_MAX) : -1;
    if (n < 0) {
        /* First boot: plant the commented default so there is something to edit. */
        if (s_plat->write_file) {
            s_plat->write_file(CONF_PATH, s_default_conf, sizeof(s_default_conf) - 1);
        }
        snprintf(text, CONF_MAX, "%s", s_default_conf);
    }
    int bad = purr_rt_config_load(&s_rt, text, r->out, sizeof(r->out));
    free(text);
    if (bad > 0) {
        ESP_LOGW(TAG, "%s: %d bad line(s)\n%s", CONF_PATH, bad, r->out);
    }
    return bad;
}

static void manager_task(void *arg)
{
    (void)arg;
    uint32_t last_tick = host_now(NULL);
    for (;;) {
        req_t *r = NULL;
        if (xQueueReceive(s_reqq, &r, pdMS_TO_TICKS(1000)) == pdTRUE && r != NULL) {
            uint32_t began = host_now(NULL);
            switch (r->kind) {
            case R_DISPATCH:  r->rc = purr_rt_dispatch(&s_rt, r->line, r->out, sizeof(r->out)); break;
            case R_QUERY:     r->rc = purr_rt_query(&s_rt, r->line, r->out, sizeof(r->out)); break;
            case R_KEY:       r->rc = purr_rt_key(&s_rt, r->mods, r->line); break;
            case R_RELOAD:    r->rc = do_reload(r); break;
            case R_OPEN_BOOT: r->rc = purr_rt_exec_boot(&s_rt); break;
            case R_OPEN_EXEC: r->rc = purr_rt_exec_once(&s_rt); break;
            case R_EMIT:      purr_rt_emit(&s_rt, r->line, r->arg); break;
            case R_POST:      r->rc = purr_rt_post(&s_rt, r->id, r->line, r->arg); break;
            case R_SUB:       r->rc = purr_rt_subscribe(&s_rt, r->id, r->line); break;
            case R_UNSUB:     purr_rt_unsubscribe(&s_rt, r->id, r->line); break;
            case R_CLOSE:     r->rc = purr_rt_stop(&s_rt, r->id, PURR_APP_DESTROY_USER); break;
            case R_FOCUS:     r->rc = purr_rt_focus(&s_rt, r->id); break;
            }
            uint32_t took = host_now(NULL) - began;
            if (took >= 1000) {
                /* a request that held the manager up: every other caller waited behind it */
                ESP_LOGW(TAG, "manager: request %d '%.40s' took %u ms", (int)r->kind, r->line,
                         (unsigned)took);
            }
            if (r->done) {
                xSemaphoreGive(r->done);
            }
            req_release(r);
        }
        uint32_t now = host_now(NULL);
        if (now - last_tick >= 1000) {
            last_tick = now;
            purr_rt_tick(&s_rt);
        }
    }
}

static volatile int s_user_apps_started;

void purr_catrt_user_logged_in(void)
{
    if (s_reqq == NULL || s_user_apps_started) {
        return;
    }
    s_user_apps_started = 1;
    post_async(R_OPEN_EXEC, NULL, NULL, 0);
}

esp_err_t purr_catrt_start(const purr_catrt_platform_t *platform)
{
    if (s_reqq != NULL || platform == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_plat = platform;
    purr_rt_host_t host = {
        .ctx = NULL, .load = host_load, .unload = host_unload, .deliver = host_deliver,
        .now_ms = host_now, .free_bytes = host_free, .save_state = host_save_state,
        .load_state = host_load_state, .log = host_log, .freeze = host_freeze, .internal_free = host_internal_free,
    };
    purr_rt_init(&s_rt, &host);
    s_rt.cfg.reserve = 64 * 1024;
    s_rt.cfg.low = 512 * 1024;
    s_rt.cfg.critical = 256 * 1024;
    s_rt.cfg.margin = 128 * 1024;
    s_rt.cfg.min_kill_ms = 5000;
    s_rt.cfg.internal_reserve = INTERNAL_RESERVE;
    s_rt.cfg.game_internal_reserve = GAME_INTERNAL_RESERVE;
    purr_rt_caps_for_psram((uint32_t)heap_caps_get_total_size(MALLOC_CAP_SPIRAM), &s_rt.cfg.max_frozen, &s_rt.cfg.max_services);
    purr_rt_set_defaults(&s_rt);
    purr_rt_add_sink(&s_rt, sink_cb, NULL);

    s_qmu = xSemaphoreCreateMutexStatic(&s_qmu_buf);
    s_reqq = xQueueCreate(16, sizeof(req_t *));
    if (s_reqq == NULL ||
        xTaskCreate(manager_task, "rt-mgr", 7168, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    char err[REPLY_LEN];
    int bad = purr_catrt_reload(err, sizeof(err));
    ESP_LOGI(TAG, "app runtime up (%s: %d bad line%s)", CONF_PATH, bad < 0 ? 0 : bad, bad == 1 ? "" : "s");
    rpc(R_OPEN_BOOT, NULL, NULL, 0, 0, NULL, 0);          /* the system's own apps, with nobody logged in */
    return ESP_OK;
}
