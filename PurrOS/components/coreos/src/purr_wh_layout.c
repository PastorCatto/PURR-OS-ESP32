/* purr_wh_layout.c - the Click Wheel shell's measures for a display (purr_wh_layout.h). */
#include "purr_wh_layout.h"

#include <stdio.h>

typedef struct {
    int w, h;
    uint8_t mono;
    purr_wh_metrics_t m;    /* w, h, mono, shape, source are filled in when it is returned */
} profile_t;

/* The pre-made sets, from the design boards (UI/wheel-draft, drawn at 2x). Order of the fields in m: see below. */
#define PROFILE(W, H, MONO, SHAPE, FROW, FTITLE, FSMALL, CLK, SPLIT, REFL, BAR, ROW, PAD, ICON, COVER, PANEL) \
    { W, H, MONO, { W, H, MONO, SHAPE, PURR_WH_FROM_PROFILE, FROW, FTITLE, FSMALL, CLK, SPLIT, REFL, BAR, ROW, ((H) - (BAR)) / (ROW), PAD, ICON, COVER, PANEL } }

static const profile_t s_profiles[] = {
    /* T-Deck, T-Deck Plus and Playtime: 320 x 240 colour. Eight rows of 26 under an 18 pixel bar (the shared W10M_STATUS_H the Type shell strip already has); a 132 pixel panel with the cover or the clock beside the list. */
    PROFILE(320, 240, 0, PURR_WH_WIDE,   PURR_WH_FONT_MEDIUM, PURR_WH_FONT_MEDIUM, PURR_WH_FONT_SMALL, 2, 1, 1, 18, 26, 8, 14, 120, 132),
    /* Waveshare ESP32-S3-ePaper-1.54: 200 x 200 black and white. Full-width rows of 22, no reflection, a smaller clock. */
    PROFILE(200, 200, 1, PURR_WH_SQUARE, PURR_WH_FONT_SMALL,  PURR_WH_FONT_MEDIUM, PURR_WH_FONT_SMALL, 1, 0, 0, 18, 22, 6, 12, 62, 0),
};

#define N_PROFILES ((int)(sizeof(s_profiles) / sizeof(s_profiles[0])))

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static const profile_t *find(int w, int h, int mono)
{
    for (int i = 0; i < N_PROFILES; i++) {
        if (s_profiles[i].w == w && s_profiles[i].h == h && s_profiles[i].mono == (mono ? 1 : 0)) {
            return &s_profiles[i];
        }
    }
    return NULL;
}

int purr_wh_has_profile(int w, int h, int mono)
{
    return find(w, h, mono) != NULL;
}

/* The rules: everything from the shorter side, in the proportions of the 320 x 240 profile (240 / 18 = bar, 240 / 26 = row, 240 / 8 = pad), so a size nobody has
 * measured gets the same look. The results for the two known sizes land on the profiles' numbers (the host test checks). */
static void from_rules(int w, int h, int mono, purr_wh_metrics_t *m)
{
    int shortside = w < h ? w : h;
    m->w = w;
    m->h = h;
    m->mono = mono ? 1 : 0;
    m->source = PURR_WH_FROM_RULES;
    if (w * 4 >= h * 5) {                                   /* at least 1.25 times as wide as tall */
        m->shape = PURR_WH_WIDE;
    } else if (h * 4 >= w * 5) {
        m->shape = PURR_WH_TALL;
    } else {
        m->shape = PURR_WH_SQUARE;
    }
    m->bar_h = clampi(shortside / 13, 16, 40);
    m->row_h = clampi(shortside / 9, 20, 44);
    m->pad = clampi(shortside / 30, 4, 14);
    m->icon = clampi(m->row_h * 14 / 26, 10, 24);
    m->rows = (h - m->bar_h) / m->row_h;
    m->font_row = m->row_h >= 30 ? PURR_WH_FONT_LARGE : (m->row_h >= 24 ? PURR_WH_FONT_MEDIUM : PURR_WH_FONT_SMALL);
    m->font_title = m->bar_h >= 30 ? PURR_WH_FONT_LARGE : PURR_WH_FONT_MEDIUM;
    m->font_small = shortside >= 320 ? PURR_WH_FONT_MEDIUM : PURR_WH_FONT_SMALL;
    m->clock_tier = shortside >= 220 ? 2 : (shortside >= 150 ? 1 : 0);
    m->split = (m->shape == PURR_WH_WIDE && w >= 280) ? 1 : 0;      /* a side panel needs a list of at least 150 pixels beside it */
    m->panel_w = m->split ? w * 33 / 80 : 0;
    m->cover = m->split ? shortside / 2 : shortside * 5 / 16;
    m->reflection = (!m->mono && m->shape == PURR_WH_WIDE) ? 1 : 0;
}

int purr_wh_layout(int w, int h, int mono, purr_wh_metrics_t *m)
{
    if (m == NULL || w < 80 || h < 80 || w > 4096 || h > 4096) {
        return -1;
    }
    const profile_t *p = find(w, h, mono);
    if (p != NULL) {
        *m = p->m;
        return 0;
    }
    from_rules(w, h, mono, m);
    return 0;
}

void purr_wh_describe(const purr_wh_metrics_t *m, char *out, int cap)
{
    static const char *const shapes[] = {"wide", "square", "tall"};
    if (m == NULL || out == NULL || cap <= 0) {
        return;
    }
    snprintf(out, (size_t)cap, "%dx%d %s %s, %s, %d rows of %d, bar %d%s", m->w, m->h, m->mono ? "mono" : "colour",
             m->shape < 3 ? shapes[m->shape] : "?", m->source == PURR_WH_FROM_PROFILE ? "profile" : "rules", m->rows, m->row_h, m->bar_h,
             m->split ? ", side panel" : "");
}
