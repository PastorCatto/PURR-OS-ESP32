/*
 * purr_wh.c - the Click Wheel look: the measures, the typefaces, the silver bar, and the playing screen (plan7.md, "The Click Wheel shell").
 *
 * The state, the keys, the focus manager and the shared sheets are the Type shell's; this file gives them an iPod-style face. Everything is sized from
 * purr_wh_layout.h (a profile for a display the project knows, rules for any other), never from a literal that belongs to one panel.
 */
#include "purr_wh_priv.h"

#if TP_WHEEL

#include <stdio.h>
#include <string.h>

#include "purr_catrt_media.h"
#include "purr_media.h"

/* ---------------------------------------------------------------- the measures */

static purr_wh_metrics_t s_m;
static int s_m_w, s_m_h, s_m_ok;

const purr_wh_metrics_t *wh_m(void)
{
    int w = w10m_disp_w(), h = w10m_disp_h();
    if (!s_m_ok || w != s_m_w || h != s_m_h) {
#ifdef CONFIG_PURR_BOARD_WAVESHARE154
        const int mono = 1;
#else
        const int mono = 0;
#endif
        if (purr_wh_layout(w, h, mono, &s_m) != 0) {
            purr_wh_layout(320, 240, 0, &s_m);              /* not a display size: the T-Deck's, rather than nothing */
        }
        s_m_w = w;
        s_m_h = h;
        s_m_ok = 1;
    }
    return &s_m;
}

const lv_font_t *wh_font(int tier)
{
    switch (tier) {
    case PURR_WH_FONT_LARGE:  return &wh_font_title;
    case PURR_WH_FONT_MEDIUM: return &wh_font_row;
    default:                  return &wh_font_small;
    }
}

lv_color_t wh_accent_light(void) { return lv_color_mix(lv_color_white(), tp_accent(), 89); }
lv_color_t wh_accent_dark(void) { return lv_color_mix(lv_color_black(), tp_accent(), 56); }

void wh_silver(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(0xF7F8FA), 0);
    lv_obj_set_style_bg_grad_color(obj, lv_color_hex(0xC8CCD3), 0);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 2, 0);
    lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(0x8A909B), 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(0x15171C), 0);
}

lv_obj_t *wh_battery(lv_obj_t *parent, int pct, int have)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(b, 22, 10);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x3B3F47), 0);
    lv_obj_set_style_radius(b, 2, 0);
    lv_obj_t *f = lv_obj_create(b);
    lv_obj_remove_style_all(f);
    lv_obj_clear_flag(f, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(f, lv_color_hex(0x3B3F47), 0);
    lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
    int fw = have ? pct * 18 / 100 : 1;
    lv_obj_set_size(f, fw < 1 ? 1 : fw, 6);
    lv_obj_align(f, LV_ALIGN_LEFT_MID, 1, 0);
    return b;
}

/* ---------------------------------------------------------------- the playing screen */

static lv_obj_t *s_pl;                                  /* its root, NULL when closed */
static lv_obj_t *s_pl_title, *s_pl_artist, *s_pl_album, *s_pl_count, *s_pl_fill, *s_pl_knob, *s_pl_track, *s_pl_t0, *s_pl_t1, *s_pl_none;
static lv_obj_t *s_pl_prev, *s_pl_play, *s_pl_next, *s_pl_art;
static lv_timer_t *s_pl_timer;
static int s_pl_track_w;

int wh_playing_now(void)
{
    purr_media_info_t i;
    return purr_media_dev_get(NULL, 0, &i) && i.state == PURR_MEDIA_PLAYING;
}

int wh_playing_is_open(void) { return s_pl != NULL; }

static void mmss(char *out, size_t cap, uint32_t ms, int neg)
{
    uint32_t s = ms / 1000;
    snprintf(out, cap, "%s%u:%02u", neg ? "-" : "", (unsigned)(s / 60), (unsigned)(s % 60));
}

static void pl_update(lv_timer_t *t)
{
    (void)t;
    if (s_pl == NULL) {
        return;
    }
    char app[PURR_MEDIA_APP];
    purr_media_info_t i;
    int have = purr_media_dev_get(app, sizeof(app), &i);
    lv_obj_t *all[] = {s_pl_art, s_pl_title, s_pl_artist, s_pl_album, s_pl_count, s_pl_track, s_pl_t0, s_pl_t1, s_pl_prev, s_pl_play, s_pl_next};
    for (size_t k = 0; k < sizeof(all) / sizeof(all[0]); k++) {
        if (have) {
            lv_obj_remove_flag(all[k], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(all[k], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!have) {
        lv_obj_remove_flag(s_pl_none, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(s_pl_none, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_pl_title, i.title[0] ? i.title : app);
    lv_label_set_text(s_pl_artist, i.artist);
    lv_label_set_text(s_pl_album, i.album);
    char c[24] = "";
    if (i.tracks > 0) {
        snprintf(c, sizeof(c), "%u of %u", (unsigned)i.track, (unsigned)i.tracks);
    }
    lv_label_set_text(s_pl_count, c);
    int fw = i.duration_ms ? (int)((uint64_t)s_pl_track_w * i.position_ms / i.duration_ms) : 0;
    lv_obj_set_width(s_pl_fill, fw < 0 ? 0 : fw);
    lv_obj_set_x(s_pl_knob, fw - 5);
    char a[12], b[12];
    mmss(a, sizeof(a), i.position_ms, 0);
    if (i.duration_ms) {
        mmss(b, sizeof(b), i.duration_ms - i.position_ms, 1);
    } else {
        b[0] = '\0';
    }
    lv_label_set_text(s_pl_t0, a);
    lv_label_set_text(s_pl_t1, b);
    lv_label_set_text(s_pl_play, i.state == PURR_MEDIA_PLAYING ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

static void cmd_clicked(lv_event_t *e)
{
    int cmd = (int)(intptr_t)lv_event_get_user_data(e);
    purr_media_dev_command(cmd, 0);
}

static lv_obj_t *ctl(lv_obj_t *parent, const char *sym, int cmd)
{
    lv_obj_t *b = tp_label(parent, sym, &lv_font_montserrat_20, NULL);
    lv_obj_set_style_text_color(b, lv_color_hex(0x3A3F48), 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(b, 10);
    lv_obj_add_event_cb(b, cmd_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)cmd);
    return b;
}

void wh_playing_open(void)
{
    if (s_pl != NULL) {
        return;
    }
    const purr_wh_metrics_t *m = wh_m();
    int w = w10m_disp_w(), h = w10m_disp_h() - TP_STRIP_H;
    int pad = m->pad + 4;
    s_pl = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_pl);
    lv_obj_clear_flag(s_pl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_pl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_pl, 0, TP_STRIP_H);
    lv_obj_set_size(s_pl, w, h);
    lv_obj_set_style_bg_color(s_pl, lv_color_white(), 0);
    lv_obj_set_style_bg_grad_color(s_pl, lv_color_hex(0xEEF1F6), 0);
    lv_obj_set_style_bg_grad_dir(s_pl, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(s_pl, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_pl, lv_color_hex(0x101216), 0);

    int cv = m->cover;                                   /* the cover: no art comes with the report yet, so a tile in the accent with a note on it */
    lv_obj_t *art = s_pl_art = lv_obj_create(s_pl);
    lv_obj_remove_style_all(art);
    lv_obj_clear_flag(art, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(art, pad, pad);
    lv_obj_set_size(art, cv, cv);
    lv_obj_set_style_radius(art, 3, 0);
    lv_obj_set_style_bg_color(art, wh_accent_light(), 0);
    lv_obj_set_style_bg_grad_color(art, wh_accent_dark(), 0);
    lv_obj_set_style_bg_grad_dir(art, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(art, LV_OPA_COVER, 0);
    lv_obj_t *note = tp_label(art, LV_SYMBOL_AUDIO, &lv_font_montserrat_20, NULL);
    lv_obj_set_style_text_color(note, lv_color_white(), 0);
    lv_obj_center(note);

    int tx = pad + cv + pad, tw = w - tx - pad;
    s_pl_title = tp_label(s_pl, "", wh_font(PURR_WH_FONT_LARGE), NULL);
    lv_label_set_long_mode(s_pl_title, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_pl_title, tx, pad);
    lv_obj_set_width(s_pl_title, tw);
    s_pl_artist = tp_label(s_pl, "", wh_font(PURR_WH_FONT_MEDIUM), NULL);
    lv_label_set_long_mode(s_pl_artist, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_pl_artist, lv_color_hex(0x3A3F48), 0);
    lv_obj_set_pos(s_pl_artist, tx, pad + 28);
    lv_obj_set_width(s_pl_artist, tw);
    s_pl_album = tp_label(s_pl, "", wh_font(PURR_WH_FONT_SMALL), NULL);
    lv_label_set_long_mode(s_pl_album, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_pl_album, lv_color_hex(0x5B616C), 0);
    lv_obj_set_pos(s_pl_album, tx, pad + 50);
    lv_obj_set_width(s_pl_album, tw);
    s_pl_count = tp_label(s_pl, "", wh_font(PURR_WH_FONT_SMALL), NULL);
    lv_obj_set_style_text_color(s_pl_count, lv_color_hex(0x7B818C), 0);
    lv_obj_set_pos(s_pl_count, tx, pad + 68);

    int ty = pad + cv + pad + 4;                         /* the progress bar under the cover, the whole width */
    s_pl_track_w = w - 2 * pad;
    s_pl_track = lv_obj_create(s_pl);
    lv_obj_remove_style_all(s_pl_track);
    lv_obj_clear_flag(s_pl_track, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_pl_track, pad, ty);
    lv_obj_set_size(s_pl_track, s_pl_track_w, 6);
    lv_obj_set_style_radius(s_pl_track, 3, 0);
    lv_obj_set_style_bg_color(s_pl_track, lv_color_hex(0xCFD4DC), 0);
    lv_obj_set_style_bg_opa(s_pl_track, LV_OPA_COVER, 0);
    s_pl_fill = lv_obj_create(s_pl_track);
    lv_obj_remove_style_all(s_pl_fill);
    lv_obj_clear_flag(s_pl_fill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_pl_fill, 0, 0);
    lv_obj_set_size(s_pl_fill, 0, 6);
    lv_obj_set_style_radius(s_pl_fill, 3, 0);
    lv_obj_set_style_bg_color(s_pl_fill, wh_accent_light(), 0);
    lv_obj_set_style_bg_grad_color(s_pl_fill, wh_accent_dark(), 0);
    lv_obj_set_style_bg_grad_dir(s_pl_fill, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(s_pl_fill, LV_OPA_COVER, 0);
    s_pl_knob = lv_obj_create(s_pl_track);
    lv_obj_remove_style_all(s_pl_knob);
    lv_obj_clear_flag(s_pl_knob, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_pl_knob, 10, 10);
    lv_obj_set_pos(s_pl_knob, -5, -2);
    lv_obj_set_style_radius(s_pl_knob, 5, 0);
    lv_obj_set_style_bg_color(s_pl_knob, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_pl_knob, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_pl_knob, 2, 0);
    lv_obj_set_style_border_color(s_pl_knob, wh_accent_dark(), 0);
    s_pl_t0 = tp_label(s_pl, "", wh_font(PURR_WH_FONT_SMALL), NULL);
    lv_obj_set_pos(s_pl_t0, pad, ty + 10);
    s_pl_t1 = tp_label(s_pl, "", wh_font(PURR_WH_FONT_SMALL), NULL);
    lv_obj_align(s_pl_t1, LV_ALIGN_TOP_RIGHT, -pad, ty + 10);

    int cy = h - 34;                                     /* the controls, centred along the bottom */
    s_pl_prev = ctl(s_pl, LV_SYMBOL_PREV, PURR_MEDIA_CMD_PREV);
    s_pl_play = ctl(s_pl, LV_SYMBOL_PLAY, PURR_MEDIA_CMD_TOGGLE);
    s_pl_next = ctl(s_pl, LV_SYMBOL_NEXT, PURR_MEDIA_CMD_NEXT);
    lv_obj_set_pos(s_pl_prev, w / 2 - 70, cy);
    lv_obj_set_pos(s_pl_play, w / 2 - 10, cy);
    lv_obj_set_pos(s_pl_next, w / 2 + 50, cy);

    s_pl_none = tp_label(s_pl, "Nothing is playing", wh_font(PURR_WH_FONT_MEDIUM), NULL);
    lv_obj_set_style_text_color(s_pl_none, lv_color_hex(0x5B616C), 0);
    lv_obj_center(s_pl_none);

    lv_obj_move_foreground(s_pl);
    pl_update(NULL);
    s_pl_timer = lv_timer_create(pl_update, 500, NULL);
    tp_strip_title("Now Playing");
}

void wh_playing_close(void)
{
    if (s_pl == NULL) {
        return;
    }
    lv_timer_delete(s_pl_timer);
    s_pl_timer = NULL;
    lv_obj_delete(s_pl);
    s_pl = NULL;
    tp_strip_title(NULL);
}

int wh_playing_key(uint32_t key)
{
    if (s_pl == NULL) {
        return 0;
    }
    switch (key) {
    case LV_KEY_ENTER: purr_media_dev_command(PURR_MEDIA_CMD_TOGGLE, 0); return 1;
    case LV_KEY_LEFT:  purr_media_dev_command(PURR_MEDIA_CMD_PREV, 0); return 1;
    case LV_KEY_RIGHT: purr_media_dev_command(PURR_MEDIA_CMD_NEXT, 0); return 1;
    case LV_KEY_UP:
    case LV_KEY_DOWN:  return 1;                         /* nothing to scroll; no volume until there is a sound path */
    default:           return 0;                         /* Back, Home and Menu go on to the shell */
    }
}

#endif /* TP_WHEEL */
