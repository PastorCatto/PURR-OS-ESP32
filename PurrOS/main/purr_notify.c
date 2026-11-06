/* purr_notify.c - see purr_notify.h. */
#include "purr_notify.h"

#include <stdio.h>
#include <string.h>

#include "esp_err.h"

#include "purr_catrt_msg.h"
#include "purr_ui_service.h"

static struct {
    char title[32];
    char text[64];
} s_log[PURR_NOTE_MAX];
static int s_n;

__attribute__((weak)) void purr_shell_notified(const char *title, const char *text) { (void)title; (void)text; }

void purr_notify_add(const char *title, const char *text)
{
    memmove(&s_log[1], &s_log[0], (PURR_NOTE_MAX - 1) * sizeof(s_log[0]));
    snprintf(s_log[0].title, sizeof(s_log[0].title), "%s", title ? title : "");
    snprintf(s_log[0].text, sizeof(s_log[0].text), "%s", text ? text : "");
    if (s_n < PURR_NOTE_MAX) {
        s_n++;
    }
    purr_shell_notified(s_log[0].title, s_log[0].text);
}

int purr_notify_count(void) { return s_n; }

int purr_notify_get(int i, char *title, size_t tcap, char *text, size_t xcap)
{
    if (i < 0 || i >= s_n) {
        return -1;
    }
    snprintf(title, tcap, "%s", s_log[i].title);
    snprintf(text, xcap, "%s", s_log[i].text);
    return 0;
}

void purr_notify_clear(void) { s_n = 0; }

typedef struct {
    char title[32], text[64];
} note_req_t;

static void *note_on_ui(void *arg)
{
    note_req_t *r = arg;
    purr_notify_add(r->title, r->text);
    return NULL;
}

/* A message arrived (purr_catrt_msg.c): called on the app's task, so it is handed to the UI task. */
void purr_msg_notify(const char *title, const char *text)
{
    note_req_t r;
    snprintf(r.title, sizeof(r.title), "%s", title ? title : "");
    snprintf(r.text, sizeof(r.text), "%s", text ? text : "");
    purr_ui_service_run(note_on_ui, &r, NULL);
}
