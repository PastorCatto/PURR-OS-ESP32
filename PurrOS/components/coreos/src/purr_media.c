/* purr_media.c - see purr_media.h. */
#include "purr_media.h"

#include <string.h>

static void copy(char *dst, int cap, const char *src)
{
    int i = 0;
    while (src != NULL && src[i] != '\0' && i < cap - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

void purr_media_init(purr_media_t *m)
{
    memset(m, 0, sizeof(*m));
}

int purr_media_report(purr_media_t *m, const char *app, const purr_media_info_t *info, uint32_t now_ms)
{
    if (m == NULL || app == NULL || app[0] == '\0' || info == NULL || info->state > PURR_MEDIA_PAUSED) {
        return -1;
    }
    if (info->duration_ms != 0 && info->position_ms > info->duration_ms) {
        return -1;
    }
    int mine = m->app[0] != '\0' && strncmp(m->app, app, PURR_MEDIA_APP - 1) == 0;
    if (m->app[0] != '\0' && !mine && m->info.state == PURR_MEDIA_PLAYING && info->state != PURR_MEDIA_PLAYING) {
        return -2;                                  /* the one that is playing keeps the buttons */
    }
    if (!mine) {
        m->ncmd = 0;                                /* commands meant for the old owner are not for this one */
        copy(m->app, PURR_MEDIA_APP, app);
    }
    m->info = *info;
    m->info.title[PURR_MEDIA_TEXT - 1] = m->info.artist[PURR_MEDIA_TEXT - 1] = m->info.album[PURR_MEDIA_TEXT - 1] = '\0';
    m->info.reserved = 0;
    m->reported_ms = now_ms;
    m->changes++;
    return 0;
}

int purr_media_clear(purr_media_t *m, const char *app)
{
    if (m == NULL || app == NULL || m->app[0] == '\0' || strncmp(m->app, app, PURR_MEDIA_APP - 1) != 0) {
        return -1;
    }
    memset(m->app, 0, sizeof(m->app));
    memset(&m->info, 0, sizeof(m->info));
    m->ncmd = 0;
    m->changes++;
    return 0;
}

int purr_media_get(const purr_media_t *m, uint32_t now_ms, char *app_out, int app_cap, purr_media_info_t *out)
{
    if (m == NULL || m->app[0] == '\0') {
        return 0;
    }
    if (app_out != NULL && app_cap > 0) {
        copy(app_out, app_cap, m->app);
    }
    if (out != NULL) {
        *out = m->info;
        if (m->info.state == PURR_MEDIA_PLAYING) {
            uint32_t pos = m->info.position_ms + (uint32_t)(now_ms - m->reported_ms);       /* wraps cleanly */
            if (m->info.duration_ms != 0 && pos > m->info.duration_ms) {
                pos = m->info.duration_ms;
            }
            out->position_ms = pos;
        }
    }
    return 1;
}

int purr_media_command(purr_media_t *m, int cmd, uint32_t arg)
{
    if (m == NULL || cmd < PURR_MEDIA_CMD_PLAY || cmd > PURR_MEDIA_CMD_STOP || m->app[0] == '\0') {
        return -1;
    }
    uint32_t need = 0;
    switch (cmd) {
    case PURR_MEDIA_CMD_PLAY:
    case PURR_MEDIA_CMD_PAUSE:
    case PURR_MEDIA_CMD_TOGGLE: need = PURR_MEDIA_CAP_PAUSE; break;
    case PURR_MEDIA_CMD_NEXT:
    case PURR_MEDIA_CMD_PREV:   need = PURR_MEDIA_CAP_SKIP; break;
    case PURR_MEDIA_CMD_SEEK:   need = PURR_MEDIA_CAP_SEEK; break;
    default: break;                                 /* STOP: every app can be asked to stop */
    }
    if ((m->info.caps & need) != need) {
        return -2;
    }
    if (m->ncmd >= PURR_MEDIA_CMDS) {
        return -3;
    }
    m->cmd[m->ncmd] = (uint8_t)cmd;
    m->cmd_arg[m->ncmd] = cmd == PURR_MEDIA_CMD_SEEK ? arg : 0;
    m->ncmd++;
    return 0;
}

int purr_media_next_command(purr_media_t *m, const char *app, int *cmd, uint32_t *arg)
{
    if (m == NULL || app == NULL || m->app[0] == '\0' || strncmp(m->app, app, PURR_MEDIA_APP - 1) != 0 || m->ncmd == 0) {
        return 0;
    }
    if (cmd != NULL) {
        *cmd = m->cmd[0];
    }
    if (arg != NULL) {
        *arg = m->cmd_arg[0];
    }
    m->ncmd--;
    for (int i = 0; i < m->ncmd; i++) {
        m->cmd[i] = m->cmd[i + 1];
        m->cmd_arg[i] = m->cmd_arg[i + 1];
    }
    return 1;
}
