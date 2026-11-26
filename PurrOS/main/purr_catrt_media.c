/* purr_catrt_media.c - see purr_catrt_media.h. */
#include "purr_catrt_media.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "purr_app_abi.h"
#include "purr_catcalls.h"
#include "purr_catrt.h"
#include "purr_catrt_power.h"
#include "purr_perm.h"

static purr_media_t s_media;
static SemaphoreHandle_t s_mu;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void lock(void) { xSemaphoreTake(s_mu, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mu); }

static void put_text(char *dst, const char *src)
{
    if (src != NULL) {
        strlcpy(dst, src, PURR_MEDIA_TEXT);
    }
}

static int cc_report(const purr_cc_media_info_t *in)
{
    if (in == NULL) {
        return PURR_MEDIA_BAD;
    }
    char app[PURR_MEDIA_APP];
    if (purr_catrt_caller(app, sizeof(app)) < 0) {
        return PURR_MEDIA_BAD;
    }
    int g = purr_catrt_perm_gate(PURR_PERM_AUDIO);      /* 0, PURR_POWER_DENIED (-2) or PURR_POWER_ASK (-3): the same answers as the power catcall */
    if (g != 0) {
        return g == -3 ? PURR_MEDIA_ASK : PURR_MEDIA_DENIED;
    }
    purr_media_info_t i;
    memset(&i, 0, sizeof(i));
    put_text(i.title, in->title);
    put_text(i.artist, in->artist);
    put_text(i.album, in->album);
    i.state = in->state;
    i.track = in->track;
    i.tracks = in->tracks;
    i.position_ms = in->position_ms;
    i.duration_ms = in->duration_ms;
    i.caps = in->caps & (PURR_MEDIA_CAP_PAUSE | PURR_MEDIA_CAP_SKIP | PURR_MEDIA_CAP_SEEK);
    lock();
    int rc = purr_media_report(&s_media, app, &i, now_ms());
    unlock();
    return rc == 0 ? PURR_MEDIA_OK : (rc == -2 ? PURR_MEDIA_BUSY : PURR_MEDIA_BAD);
}

static void cc_clear(void)
{
    char app[PURR_MEDIA_APP];
    if (purr_catrt_caller(app, sizeof(app)) >= 0) {
        lock();
        purr_media_clear(&s_media, app);
        unlock();
    }
}

static int cc_next_command(int *cmd, uint32_t *arg)
{
    char app[PURR_MEDIA_APP];
    if (purr_catrt_caller(app, sizeof(app)) < 0) {
        return 0;
    }
    lock();
    int r = purr_media_next_command(&s_media, app, cmd, arg);
    unlock();
    return r;
}

static const purr_cc_media_t s_cc_media = {.version = 1, .report = cc_report, .clear = cc_clear, .next_command = cc_next_command};

void purr_catrt_media_init(void)
{
    if (s_mu == NULL) {
        s_mu = xSemaphoreCreateMutex();
        purr_media_init(&s_media);
    }
    purr_catrt_set_media(&s_cc_media);
}

int purr_media_dev_get(char *app, size_t app_cap, purr_media_info_t *out)
{
    if (s_mu == NULL) {
        return 0;
    }
    char who[PURR_MEDIA_APP];
    lock();
    int r = purr_media_get(&s_media, now_ms(), who, sizeof(who), out);
    unlock();
    if (r && purr_catrt_slot_of(who) < 0) {             /* the app stopped without clearing: the session goes with it */
        lock();
        purr_media_clear(&s_media, who);
        unlock();
        return 0;
    }
    if (r && app != NULL && app_cap > 0) {
        strlcpy(app, who, app_cap);
    }
    return r;
}

int purr_media_dev_command(int cmd, uint32_t arg)
{
    if (s_mu == NULL) {
        return -1;
    }
    char who[PURR_MEDIA_APP];
    lock();
    int r = purr_media_command(&s_media, cmd, arg);
    purr_media_get(&s_media, now_ms(), who, sizeof(who), NULL);
    unlock();
    if (r == 0) {
        int slot = purr_catrt_slot_of(who);
        if (slot >= 0) {
            purr_catrt_post_event(slot, PURR_APPEV_USER, 0, "media.cmd");
        }
    }
    return r;
}

uint32_t purr_media_dev_changes(void)
{
    uint32_t c = 0;
    if (s_mu != NULL) {
        lock();
        c = s_media.changes;
        unlock();
    }
    return c;
}

int purr_media_dev_open_owner(void)
{
    char who[PURR_MEDIA_APP];
    if (!purr_media_dev_get(who, sizeof(who), NULL)) {
        return -1;
    }
    int slot = purr_catrt_slot_of(who);
    if (slot < 0) {
        return -1;
    }
    purr_catrt_focus_slot(slot);
    return 0;
}
