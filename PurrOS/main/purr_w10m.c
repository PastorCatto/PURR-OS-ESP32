/*
 * purr_w10m.c - a Windows 10 Mobile style shell (purr_w10m.h, plan7.md).
 *
 * Everything here runs on the UI service task, the only place LVGL may be called from. The
 * entry points that other tasks use (purr_w10m_start, purr_w10m_set_orientation) hop over with
 * purr_ui_service_run().
 *
 * Layout: one packing routine places tiles on a grid whose short side holds three cells. In
 * landscape that is three rows that scroll sideways; in portrait it is three columns that scroll
 * downwards. A change of orientation re-runs the same routine, so there is one layout to keep
 * right, not two.
 */
#include "purr_w10m.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sdkconfig.h"

#include "commands.h"
#include "login.h"
#include "purr_appmgr.h"
#include "purr_catrt.h"
#include "purr_catrt_ui.h"
#include "purr_console.h"
#include "purr_kernel.h"
#include "purr_settings.h"
#include "purr_ui_config.h"
#include "purr_lvgl.h"
#include "purr_ui_assets.h"
#include "purr_ui_service.h"
#include "purr_w10m_priv.h"
#include "purr_w10m_theme.h"

static const char *TAG = "w10m";

/* ---------------------------------------------------------------- geometry */

#define STATUS_H     18                 /* the status strip, always on top */
#define CELL         70                 /* one small tile, in pixels */
#define GAP          4
#define PITCH        (CELL + GAP)
#define PAD          6                  /* around the tile grid */
#define SHORT_CELLS  3                  /* cells across the short side */
#define MAX_TILES    40

/* The palette and the look live in purr_w10m_theme.c. */
static const char *s_wallpaper = "img0";

/* ---------------------------------------------------------------- assets as LVGL images */

typedef struct {
    char name[24];
    lv_image_dsc_t dsc;
    int ok;
} img_slot_t;

static img_slot_t s_imgs[128];
static int s_nimgs;

/* The original art, as exported: LVGL native RGB565, or RGB565 with a separate alpha plane. The
 * pixels stay in the mapped flash partition; only this small descriptor is built. NULL if the
 * asset is missing, in which case callers draw without it. */
static const lv_image_dsc_t *img(const char *name)
{
    for (int i = 0; i < s_nimgs; i++) {
        if (strcmp(s_imgs[i].name, name) == 0) {
            return s_imgs[i].ok ? &s_imgs[i].dsc : NULL;
        }
    }
    if (s_nimgs >= (int)(sizeof(s_imgs) / sizeof(s_imgs[0]))) {
        return NULL;
    }
    img_slot_t *s = &s_imgs[s_nimgs++];
    snprintf(s->name, sizeof(s->name), "%s", name);
    const purr_asset_t *a = purr_ui_assets_find(name);
    if (a == NULL) {
        s->ok = 0;
        return NULL;
    }
    int alpha = a->color_format == PURR_ASSET_RGB565A8;
    s->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s->dsc.header.cf = alpha ? LV_COLOR_FORMAT_RGB565A8 : LV_COLOR_FORMAT_RGB565;
    s->dsc.header.flags = 0;
    s->dsc.header.w = a->width;
    s->dsc.header.h = a->height;
    s->dsc.header.stride = (uint32_t)a->width * 2;
    s->dsc.data_size = (uint32_t)a->width * a->height * (alpha ? 3 : 2);
    s->dsc.data = (const uint8_t *)a->pixels;
    s->ok = 1;
    return &s->dsc;
}

/* ---------------------------------------------------------------- tiles */

typedef struct {
    char name[PURR_APP_NAME_LEN];
    const char *icon;                   /* asset name, or NULL for a letter tile */
    const char *live;                   /* a live tile's art, or NULL */
    int wide;                           /* 1 = two cells wide */
    int vertical;                       /* live tile flips vertically (else horizontally) */
    int system;                         /* built into the shell: 1 = Settings, 2 = the Developer Portal */
    int dev;                            /* a developer app (devapps folder, or unsigned): shown with a DEV mark */
} tile_def_t;

/* What the start screen shows: the shell's own Settings, then every app installed for the signed-in
 * user that verifies. Filled by load_defs(); there are no placeholder tiles. */
static tile_def_t s_tile_defs[MAX_TILES];
static int s_ndefs;
#define N_TILE_DEFS s_ndefs

typedef struct {
    lv_obj_t *obj;
    const tile_def_t *def;
    int cx, cy;                         /* cell position, set by the packer */
} tile_t;

static tile_t s_tiles[MAX_TILES];
static int s_ntiles;

/* ---------------------------------------------------------------- the shell's objects */

static lv_obj_t *s_wall;                /* the wallpaper, behind everything */
static lv_obj_t *s_shade;               /* a dark veil over it so the tiles read */
static lv_obj_t *s_start;               /* the scrolling start screen */
static lv_obj_t *s_status;              /* the status strip */
static lv_obj_t *s_clock;
static lv_obj_t *s_status_dev;          /* "DEV" / "UNSIGNED OK" badge while developer mode is active */
static lv_obj_t *s_status_user;         /* the signed-in user's name, at the left of the strip */
static lv_obj_t *s_net;
static lv_obj_t *s_allapps;             /* the round button at the end of the tiles */
static lv_obj_t *s_allapps_circle, *s_allapps_arrow, *s_allapps_label;

static int disp_w(void) { return (int)lv_display_get_horizontal_resolution(NULL); }
static int disp_h(void) { return (int)lv_display_get_vertical_resolution(NULL); }
static int is_landscape(void) { return disp_w() >= disp_h(); }

const lv_image_dsc_t *w10m_img(const char *name) { return img(name); }
int w10m_disp_w(void) { return disp_w(); }
int w10m_disp_h(void) { return disp_h(); }

#define accent() purr_theme_accent_color()

/* ---------------------------------------------------------------- the tile packer */

/* Places each tile on the first free spot, scanning along the short side first: down the rows in
 * landscape (so the screen fills column by column and scrolls sideways) and across the columns in
 * portrait. A wide tile needs two free cells side by side across the short side's axis, so it
 * takes a whole row slot in landscape. */
static int occupied(const uint8_t *grid, int cols, int rows, int cx, int cy)
{
    if (cx < 0 || cy < 0 || cx >= cols || cy >= rows) {
        return 1;
    }
    return grid[cy * cols + cx];
}

static void pack_tiles(void)
{
    static uint8_t grid[16 * 16];
    memset(grid, 0, sizeof(grid));
    int land = is_landscape();
    int cols = land ? 14 : SHORT_CELLS;       /* the long side is as long as it needs to be */
    int rows = land ? SHORT_CELLS : 14;
    if (cols > 16) cols = 16;
    if (rows > 16) rows = 16;

    for (int i = 0; i < s_ntiles; i++) {
        tile_t *t = &s_tiles[i];
        int w = t->def->wide ? 2 : 1;
        int placed = 0;
        /* major axis = the long side, minor = the short side */
        int major_n = land ? cols : rows;
        int minor_n = land ? rows : cols;
        for (int maj = 0; maj < major_n && !placed; maj++) {
            for (int min = 0; min < minor_n && !placed; min++) {
                int cx = land ? maj : min;
                int cy = land ? min : maj;
                int ok = 1;
                for (int k = 0; k < w && ok; k++) {
                    /* a wide tile spreads across x in both orientations (it is wide on screen) */
                    ok = !occupied(grid, cols, rows, cx + k, cy);
                }
                if (ok) {
                    for (int k = 0; k < w; k++) {
                        grid[cy * cols + cx + k] = 1;
                    }
                    t->cx = cx;
                    t->cy = cy;
                    placed = 1;
                }
            }
        }
        if (!placed) {
            t->cx = t->cy = -1;
        }
    }
}

/* The round "All apps" button goes straight after the last tile: past the last column in
 * landscape (centred between the rows), under the last row in portrait. It scrolls with the tiles,
 * so reaching the end of them is how you find it. */
static void position_allapps(void)
{
    if (s_allapps == NULL) {
        return;
    }
    int maxcx = -1, maxcy = -1;
    for (int i = 0; i < s_ntiles; i++) {
        const tile_t *t = &s_tiles[i];
        if (t->cx < 0) {
            continue;
        }
        int w = t->def->wide ? 2 : 1;
        if (t->cx + w - 1 > maxcx) maxcx = t->cx + w - 1;
        if (t->cy > maxcy) maxcy = t->cy;
    }
    if (is_landscape()) {
        lv_obj_set_size(s_allapps, 64, SHORT_CELLS * PITCH - GAP);
        lv_obj_set_pos(s_allapps, PAD + (maxcx + 1) * PITCH + 4, PAD);
        lv_obj_align(s_allapps_circle, LV_ALIGN_CENTER, 0, -8);
        lv_label_set_text(s_allapps_arrow, LV_SYMBOL_RIGHT);
        lv_obj_align(s_allapps_label, LV_ALIGN_CENTER, 0, 24);
    } else {
        lv_obj_set_size(s_allapps, SHORT_CELLS * PITCH - GAP, 52);
        lv_obj_set_pos(s_allapps, PAD, PAD + (maxcy + 1) * PITCH + 4);
        lv_obj_align(s_allapps_circle, LV_ALIGN_LEFT_MID, 8, 0);
        lv_label_set_text(s_allapps_arrow, LV_SYMBOL_DOWN);
        lv_obj_align(s_allapps_label, LV_ALIGN_LEFT_MID, 62, 0);
    }
}

static void position_tiles(void)
{
    for (int i = 0; i < s_ntiles; i++) {
        tile_t *t = &s_tiles[i];
        if (t->cx < 0) {
            lv_obj_add_flag(t->obj, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(t->obj, LV_OBJ_FLAG_HIDDEN);
        int w = t->def->wide ? CELL * 2 + GAP : CELL;
        lv_obj_set_size(t->obj, w, CELL);
        lv_obj_set_pos(t->obj, PAD + t->cx * PITCH, PAD + t->cy * PITCH);
    }
    position_allapps();
}

/* ---------------------------------------------------------------- building the tiles */

/* The runtime's `open` can wait on the app's own start-up callback, so it is never called on the UI
 * task: a short task makes the request and reports back through the control center. */
typedef struct {
    char name[PURR_APP_NAME_LEN];
} open_job_t;

static void open_task(void *arg)
{
    open_job_t *j = arg;
    char line[PURR_APP_NAME_LEN + 8], out[160];
    snprintf(line, sizeof(line), "open %s", j->name);
    out[0] = '\0';
    int rc = purr_catrt_dispatch(line, out, sizeof(out));
    if (rc != 0) {
        char msg[200];
        snprintf(msg, sizeof(msg), "%s", out[0] ? out : "could not start");
        purr_w10m_notify(j->name, msg);
    }
    free(j);
    vTaskDelete(NULL);
}

static void open_def(const tile_def_t *d)
{
    if (d->system == 2) {
        w10m_devportal_open();
        return;
    }
    if (d->system) {
        w10m_settings_open();
        return;
    }
    open_job_t *j = malloc(sizeof(*j));
    if (j == NULL) {
        return;
    }
    snprintf(j->name, sizeof(j->name), "%s", d->name);
    if (xTaskCreate(open_task, "w10m-open", 4096, j, 4, NULL) != pdPASS) {
        free(j);
    }
}

static void tile_clicked(lv_event_t *e)
{
    tile_t *t = lv_event_get_user_data(e);
    ESP_LOGI(TAG, "tile '%s' tapped", t->def->name);
    open_def(t->def);
}

static lv_obj_t *make_tile(lv_obj_t *parent, tile_t *t)
{
    const tile_def_t *d = t->def;
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    purr_theme_apply(o, PURR_TH_TILE);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(o, tile_clicked, LV_EVENT_CLICKED, t);
    /* a touch dims the tile a little, as the original does (the tile style carries that) */

    const lv_image_dsc_t *icon = d->icon ? img(d->icon) : NULL;
    if (icon != NULL) {
        lv_obj_t *im = lv_image_create(o);
        lv_image_set_src(im, icon);
        lv_obj_align(im, LV_ALIGN_CENTER, 0, -6);
        lv_obj_remove_flag(im, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_t *l = lv_label_create(o);
        char c[2] = {d->name[0], 0};
        lv_label_set_text(l, c);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, -6);
    }
    if (d->dev) {
        lv_obj_t *mark = lv_label_create(o);
        lv_label_set_text(mark, "DEV");
        lv_obj_set_style_text_font(mark, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(mark, lv_color_hex(0xFFB900), 0);
        lv_obj_align(mark, LV_ALIGN_TOP_RIGHT, -3, 1);
    }
    lv_obj_t *name = lv_label_create(o);
    lv_label_set_text(name, d->name);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(name, lv_color_white(), 0);
    lv_obj_align(name, LV_ALIGN_BOTTOM_LEFT, 4, -3);
    return o;
}

static void relayout(void);
static void theme_changed(void);
static void make_tiles(void);
static void apps_refresh(void);
static void lock_show(void);
static void list_layout(void);
static void list_move(int open);
static void lock_layout(void);
static void bfu_layout(void);

/* ---------------------------------------------------------------- the status strip */

/* 24-hour or 12-hour, from the time.clock24 setting (Settings > Time & Language). */
static void fmt_hm(const struct tm *tm, char *buf, size_t cap)
{
    if (purr_settings_kv_int("time.clock24", 1)) {
        snprintf(buf, cap, "%02d:%02d", tm->tm_hour, tm->tm_min);
    } else {
        int h = tm->tm_hour % 12;
        snprintf(buf, cap, "%d:%02d %s", h == 0 ? 12 : h, tm->tm_min, tm->tm_hour < 12 ? "AM" : "PM");
    }
}

static void clock_tick(lv_timer_t *timer)
{
    (void)timer;
    time_t now = time(NULL);
    char buf[12];
    if (now > 1000000000) {
        struct tm tm;
        localtime_r(&now, &tm);
        fmt_hm(&tm, buf, sizeof(buf));
    } else {
        snprintf(buf, sizeof(buf), "--:--");     /* the clock has not been set */
    }
    lv_label_set_text(s_clock, buf);
}

void w10m_dev_badge_update(void)
{
    if (s_status_dev == NULL) {
        return;
    }
    if (purr_security_unsigned_apps_allowed()) {
        lv_label_set_text(s_status_dev, "UNSIGNED APPS OK");
        lv_obj_remove_flag(s_status_dev, LV_OBJ_FLAG_HIDDEN);
    } else if (purr_security_dev_active()) {
        lv_label_set_text(s_status_dev, "DEV");
        lv_obj_remove_flag(s_status_dev, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_status_dev, LV_OBJ_FLAG_HIDDEN);
    }
}

static void build_status(void)
{
    lv_obj_t *top = lv_layer_top();
    s_status = lv_obj_create(top);
    lv_obj_remove_style_all(s_status);
    lv_obj_set_size(s_status, LV_PCT(100), STATUS_H);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 0);
    purr_theme_apply(s_status, PURR_TH_STRIP);
    lv_obj_clear_flag(s_status, LV_OBJ_FLAG_SCROLLABLE);

    s_clock = lv_label_create(s_status);
    lv_obj_set_style_text_font(s_clock, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_clock, lv_color_white(), 0);
    lv_obj_align(s_clock, LV_ALIGN_RIGHT_MID, -6, 0);

    const lv_image_dsc_t *net = img("wifi_icon");
    if (net != NULL) {
        s_net = lv_image_create(s_status);
        lv_image_set_src(s_net, net);
        lv_image_set_scale(s_net, 160);          /* the icons are drawn for a bigger panel */
        lv_obj_align(s_net, LV_ALIGN_RIGHT_MID, -48, 0);
    }
    s_status_user = lv_label_create(s_status);
    lv_label_set_text(s_status_user, purr_login_current()->name[0] ? purr_login_current()->name : "");
    lv_obj_set_style_text_font(s_status_user, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_status_user, lv_color_white(), 0);
    lv_obj_align(s_status_user, LV_ALIGN_LEFT_MID, 6, 0);

    s_status_dev = lv_label_create(s_status);
    lv_obj_set_style_text_font(s_status_dev, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_status_dev, lv_color_hex(0xFFB900), 0);
    lv_obj_align(s_status_dev, LV_ALIGN_CENTER, 0, 0);
    w10m_dev_badge_update();

    clock_tick(NULL);
    lv_timer_create(clock_tick, 1000, NULL);
}

/* ---------------------------------------------------------------- the control center
 *
 * The notification panel of the original: a full-screen sheet you pull down from the top. A clock
 * on top, the quick-action tiles, then the notifications. Tiles for things this hardware or OS
 * cannot do yet (cellular, Bluetooth, airplane mode, hotspot, VPN, location) are drawn dimmed and
 * do nothing, rather than pretending to work. */

#define CC_ACTION_H   48
#define CC_PER_ROW_LANDSCAPE 5          /* tiles shown while collapsed, and per row when expanded */
#define CC_PER_ROW_PORTRAIT  4
#define CC_MAX_NOTES  8

static lv_obj_t *s_cc;                  /* the sheet */
static lv_obj_t *s_cc_time, *s_cc_date;
static lv_obj_t *s_cc_actions;          /* the tile grid */
static lv_obj_t *s_cc_notes;            /* the notification list */
static lv_obj_t *s_cc_empty;            /* "no new notifications" */
static lv_obj_t *s_cc_clear;            /* "Clear all" */
static lv_obj_t *s_cc_expand;           /* the expand / collapse button */
static lv_obj_t *s_cc_expand_lbl;
static lv_obj_t *s_cc_lock;             /* "Lock" */
static int s_cc_expanded;               /* 0 = one row of tiles, 1 = all of them */
static lv_obj_t *s_cc_handle;           /* grab bar along the bottom edge: drag up to close */
static int s_cc_h, s_cc_open;
static int s_press_y0, s_dragged, s_from_open;
static int s_brightness_step = 3;       /* index into s_brightness_levels */
static const uint8_t s_brightness_levels[] = {40, 110, 180, 255};

typedef struct {
    const char *name;
    const char *icon;
    int enabled;                        /* 0 = not supported here yet: shown dimmed, does nothing */
    int checkable;
    int on;
    void (*act)(int on);
    lv_obj_t *obj;
    lv_obj_t *label;
} quick_t;

static void act_brightness(int on);
static void act_rotate(int on);
static void act_settings(int on);
static void cc_move(int open);

static quick_t s_quick[] = {
    {"Network",    "wifi_ic",       1, 1, 1, NULL,           NULL, NULL},
    {"Brightness", "brightness_ic", 1, 0, 0, act_brightness, NULL, NULL},
    {"Rotate",     NULL,            1, 0, 0, act_rotate,     NULL, NULL},
    {"Settings",   "settings_ic",   1, 0, 0, act_settings,   NULL, NULL},
    {"Airplane",   "airplane_ic",   0, 1, 0, NULL,           NULL, NULL},
    {"Cellular",   "cellular_ic",   0, 1, 0, NULL,           NULL, NULL},
    {"Bluetooth",  "bluetooth_ic",  0, 1, 0, NULL,           NULL, NULL},
    {"Battery",    "battery_ic",    0, 1, 0, NULL,           NULL, NULL},
    {"Hotspot",    "hotspot_ic",    0, 1, 0, NULL,           NULL, NULL},
    {"VPN",        "vpn_ic",        0, 1, 0, NULL,           NULL, NULL},
    {"Location",   "location_ic",   0, 1, 0, NULL,           NULL, NULL},
};
#define N_QUICK ((int)(sizeof(s_quick) / sizeof(s_quick[0])))

static int s_brightness = 255;

void w10m_set_brightness(int level)
{
    level = level < 1 ? 1 : (level > 255 ? 255 : level);
    s_brightness = level;
    const purr_display_v2_t *d = purr_kernel_display();
    if (d != NULL && d->set_brightness != NULL) {
        d->set_brightness((uint8_t)level);
    }
}

int w10m_get_brightness(void) { return s_brightness; }

void w10m_save_display(void)
{
    char v[12];
    snprintf(v, sizeof(v), "%d", s_brightness);
    purr_settings_kv_set("display.brightness", v);
}

static void act_brightness(int on)
{
    (void)on;
    s_brightness_step = (s_brightness_step + 1) % (int)sizeof(s_brightness_levels);
    w10m_set_brightness(s_brightness_levels[s_brightness_step]);
    ESP_LOGI(TAG, "brightness %d", s_brightness_levels[s_brightness_step]);
}

static void act_rotate(int on)
{
    (void)on;
    int next = (purr_lvgl_get_rotation() + 90) % 360;
    w10m_apply_orientation(next);
}

static void act_settings(int on)
{
    (void)on;
    cc_move(0);
    w10m_settings_open();
}

static void quick_refresh(quick_t *q)
{
    if (q->obj == NULL) {
        return;
    }
    lv_color_t bg = (q->enabled && q->checkable && q->on) ? accent()
                  : (q->enabled ? lv_color_hex(0x404040) : lv_color_hex(0x262626));
    lv_obj_set_style_bg_color(q->obj, bg, 0);
    lv_obj_set_style_opa(q->obj, q->enabled ? LV_OPA_COVER : LV_OPA_50, 0);
}

static void quick_clicked(lv_event_t *e)
{
    quick_t *q = lv_event_get_user_data(e);
    if (!q->enabled) {
        return;
    }
    if (q->checkable) {
        q->on = !q->on;
    }
    if (q->act) {
        q->act(q->on);
    }
    quick_refresh(q);
}

static void theme_changed(void)
{
    for (int i = 0; i < N_QUICK; i++) {
        quick_refresh(&s_quick[i]);
    }
}

static void cc_clock_refresh(void)
{
    time_t now = time(NULL);
    if (now > 1000000000) {
        struct tm tm;
        localtime_r(&now, &tm);
        char tb[12], db[24];
        fmt_hm(&tm, tb, sizeof(tb));
        strftime(db, sizeof(db), "%a %d %b", &tm);
        lv_label_set_text(s_cc_time, tb);
        lv_label_set_text(s_cc_date, db);
    } else {
        lv_label_set_text(s_cc_time, "--:--");
        lv_label_set_text(s_cc_date, "clock not set");
    }
}

static void cc_anim_y_cb(void *obj, int32_t v) { lv_obj_set_y((lv_obj_t *)obj, v); }

static void cc_anim_done(lv_anim_t *a)
{
    (void)a;
    if (!s_cc_open) {
        lv_obj_add_flag(s_cc, LV_OBJ_FLAG_HIDDEN);       /* nothing to draw while it is shut */
    }
}

static void cc_move(int open)
{
    s_cc_open = open;
    if (open) {
        cc_clock_refresh();
        lv_obj_remove_flag(s_cc, LV_OBJ_FLAG_HIDDEN);
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_cc);
    lv_anim_set_exec_cb(&a, cc_anim_y_cb);
    lv_anim_set_values(&a, lv_obj_get_y(s_cc), open ? 0 : -s_cc_h);
    lv_anim_set_duration(&a, 220);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, cc_anim_done);
    lv_anim_start(&a);
}

/* One handler for both grab areas (the status strip pulls it down, the bottom bar pushes it up):
 * the sheet follows the finger, and on release it settles open or shut depending on how far it
 * went. A touch that never moved toggles it. */
static void cc_drag_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    if (in == NULL) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(in, &p);
    if (code == LV_EVENT_PRESSED) {
        lv_anim_delete(s_cc, cc_anim_y_cb);
        s_press_y0 = p.y;
        s_dragged = 0;
        s_from_open = s_cc_open;
    } else if (code == LV_EVENT_PRESSING) {
        int dy = p.y - s_press_y0;
        if (dy > 5 || dy < -5) {
            s_dragged = 1;
        }
        if (s_dragged) {
            int base = s_from_open ? 0 : -s_cc_h;
            int y = base + dy;
            if (y > 0) y = 0;
            if (y < -s_cc_h) y = -s_cc_h;
            lv_obj_remove_flag(s_cc, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_y(s_cc, y);
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (!s_dragged) {
            if (code == LV_EVENT_RELEASED) {
                cc_move(!s_from_open);
            }
        } else {
            int y = lv_obj_get_y(s_cc);
            cc_move(s_from_open ? (y > -s_cc_h / 3) : (y > -s_cc_h * 2 / 3));
        }
    }
}

/* Notifications: newest on top, tap one to dismiss it. Called on the UI task. */
static void note_clicked(lv_event_t *e)
{
    lv_obj_t *n = lv_event_get_target(e);
    lv_obj_delete(n);
    if (lv_obj_get_child_count(s_cc_notes) == 0) {
        lv_obj_remove_flag(s_cc_empty, LV_OBJ_FLAG_HIDDEN);
    }
}

static void add_note(const char *title, const char *text)
{
    while (lv_obj_get_child_count(s_cc_notes) >= CC_MAX_NOTES) {
        lv_obj_delete(lv_obj_get_child(s_cc_notes, lv_obj_get_child_count(s_cc_notes) - 1));
    }
    lv_obj_t *n = lv_obj_create(s_cc_notes);
    lv_obj_remove_style_all(n);
    lv_obj_set_width(n, LV_PCT(100));
    lv_obj_set_height(n, LV_SIZE_CONTENT);
    purr_theme_apply(n, PURR_TH_SURFACE);
    purr_theme_apply(n, PURR_TH_ACCENT_BORDER);
    lv_obj_set_style_pad_all(n, 5, 0);
    lv_obj_set_style_pad_row(n, 1, 0);
    lv_obj_set_flex_flow(n, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(n, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(n, note_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *a = lv_label_create(n);
    lv_label_set_text(a, title);
    lv_obj_set_style_text_font(a, &lv_font_montserrat_12, 0);
    purr_theme_apply(a, PURR_TH_TEXT);
    lv_obj_t *b = lv_label_create(n);
    lv_label_set_text(b, text);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(b, LV_PCT(100));
    lv_obj_set_style_text_font(b, &lv_font_montserrat_12, 0);
    purr_theme_apply(b, PURR_TH_DIM);
    lv_obj_move_to_index(n, 0);
    lv_obj_add_flag(s_cc_empty, LV_OBJ_FLAG_HIDDEN);
}

static void clear_notes(lv_event_t *e)
{
    (void)e;
    lv_obj_clean(s_cc_notes);
    lv_obj_remove_flag(s_cc_empty, LV_OBJ_FLAG_HIDDEN);
}

static int cc_per_row(void)
{
    return is_landscape() ? CC_PER_ROW_LANDSCAPE : CC_PER_ROW_PORTRAIT;
}

static void cc_layout(void)
{
    int w = disp_w(), h = disp_h();
    s_cc_h = h;
    lv_obj_set_size(s_cc, w, h);
    lv_obj_set_y(s_cc, s_cc_open ? 0 : -h);
    if (!s_cc_open) {
        lv_obj_add_flag(s_cc, LV_OBJ_FLAG_HIDDEN);
    }
    /* below the status strip: the clock and date on the left, the expand button on the right, the
     * tile grid, the notification list, and the handle along the bottom */
    int top = STATUS_H + 2;
    lv_obj_align(s_cc_time, LV_ALIGN_TOP_LEFT, 8, top);
    lv_obj_align(s_cc_date, LV_ALIGN_TOP_LEFT, 10, top + 32);
    lv_obj_align(s_cc_expand, LV_ALIGN_TOP_RIGHT, -4, top + 6);
    lv_obj_align_to(s_cc_lock, s_cc_expand, LV_ALIGN_OUT_LEFT_MID, -2, 0);

    /* Collapsed it shows one row; expanded, every tile, in rows of the same length. The tile width
     * is worked out from the row length so a row always fills the width exactly. */
    int per_row = cc_per_row();
    int visible = s_cc_expanded ? N_QUICK : (per_row < N_QUICK ? per_row : N_QUICK);
    int tile_w = (w - 8 - (per_row - 1) * 4) / per_row;
    for (int i = 0; i < N_QUICK; i++) {
        quick_t *q = &s_quick[i];
        lv_obj_set_size(q->obj, tile_w, CC_ACTION_H);
        if (i < visible) {
            lv_obj_remove_flag(q->obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(q->obj, LV_OBJ_FLAG_HIDDEN);        /* hidden objects leave the flex flow */
        }
    }
    int rows = (visible + per_row - 1) / per_row;
    int grid_h = rows * (CC_ACTION_H + 4) - 4;
    lv_obj_set_size(s_cc_actions, w - 8, grid_h);
    lv_obj_align(s_cc_actions, LV_ALIGN_TOP_MID, 0, top + 52);
    lv_label_set_text(s_cc_expand_lbl, s_cc_expanded ? LV_SYMBOL_UP : LV_SYMBOL_DOWN);

    int notes_top = top + 52 + grid_h + 18;
    int notes_h = h - notes_top - 16;
    if (notes_h < 20) notes_h = 20;
    lv_obj_set_size(s_cc_notes, w - 8, notes_h);
    lv_obj_align(s_cc_notes, LV_ALIGN_TOP_MID, 0, notes_top);
    lv_obj_align(s_cc_empty, LV_ALIGN_TOP_MID, 0, notes_top + 4);
    lv_obj_align(s_cc_clear, LV_ALIGN_TOP_RIGHT, -8, notes_top - 14);
    lv_obj_set_size(s_cc_handle, w, 16);
    lv_obj_align(s_cc_handle, LV_ALIGN_BOTTOM_MID, 0, 0);
}

static void lock_clicked(lv_event_t *e)
{
    (void)e;
    lock_show();
}

static void expand_clicked(lv_event_t *e)
{
    (void)e;
    s_cc_expanded = !s_cc_expanded;
    cc_layout();
}

static void build_cc(void)
{
    s_cc = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_cc);
    purr_theme_apply(s_cc, PURR_TH_SHEET);
    lv_obj_clear_flag(s_cc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_cc, LV_OBJ_FLAG_CLICKABLE);       /* it swallows touches meant for what is beneath */
    lv_obj_add_flag(s_cc, LV_OBJ_FLAG_HIDDEN);

    s_cc_time = lv_label_create(s_cc);
    lv_obj_set_style_text_font(s_cc_time, &lv_font_montserrat_28, 0);
    purr_theme_apply(s_cc_time, PURR_TH_TEXT);
    s_cc_date = lv_label_create(s_cc);
    lv_obj_set_style_text_font(s_cc_date, &lv_font_montserrat_12, 0);
    purr_theme_apply(s_cc_date, PURR_TH_DIM);

    s_cc_expand = lv_obj_create(s_cc);
    lv_obj_remove_style_all(s_cc_expand);
    lv_obj_set_size(s_cc_expand, 40, 36);
    lv_obj_add_flag(s_cc_expand, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_cc_expand, expand_clicked, LV_EVENT_CLICKED, NULL);
    s_cc_expand_lbl = lv_label_create(s_cc_expand);
    lv_label_set_text(s_cc_expand_lbl, LV_SYMBOL_DOWN);
    purr_theme_apply(s_cc_expand_lbl, PURR_TH_TEXT);
    lv_obj_align(s_cc_expand_lbl, LV_ALIGN_CENTER, 0, 0);

    s_cc_lock = lv_label_create(s_cc);
    lv_label_set_text(s_cc_lock, "Lock");
    lv_obj_set_style_text_font(s_cc_lock, &lv_font_montserrat_14, 0);
    purr_theme_apply(s_cc_lock, PURR_TH_ACCENT_TEXT);
    lv_obj_add_flag(s_cc_lock, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_cc_lock, 10);
    lv_obj_add_event_cb(s_cc_lock, lock_clicked, LV_EVENT_CLICKED, NULL);

    s_cc_actions = lv_obj_create(s_cc);
    lv_obj_remove_style_all(s_cc_actions);
    lv_obj_clear_flag(s_cc_actions, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_cc_actions, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(s_cc_actions, 4, 0);
    lv_obj_set_style_pad_column(s_cc_actions, 4, 0);
    for (int i = 0; i < N_QUICK; i++) {
        quick_t *q = &s_quick[i];
        q->obj = lv_obj_create(s_cc_actions);
        lv_obj_remove_style_all(q->obj);
        lv_obj_set_size(q->obj, 56, CC_ACTION_H);
        lv_obj_set_style_bg_opa(q->obj, LV_OPA_COVER, 0);
        lv_obj_clear_flag(q->obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(q->obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(q->obj, quick_clicked, LV_EVENT_CLICKED, q);
        const lv_image_dsc_t *ic = q->icon ? img(q->icon) : NULL;
        if (ic != NULL) {
            lv_obj_t *im = lv_image_create(q->obj);
            lv_image_set_src(im, ic);
            lv_obj_align(im, LV_ALIGN_TOP_MID, 0, 4);
            lv_obj_remove_flag(im, LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_obj_t *l = lv_label_create(q->obj);
            lv_label_set_text(l, LV_SYMBOL_REFRESH);
            lv_obj_set_style_text_color(l, lv_color_white(), 0);
            lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 5);
        }
        q->label = lv_label_create(q->obj);
        lv_label_set_text(q->label, q->name);
        lv_obj_set_style_text_font(q->label, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(q->label, lv_color_white(), 0);
        lv_obj_align(q->label, LV_ALIGN_BOTTOM_MID, 0, -2);
        quick_refresh(q);
    }

    s_cc_notes = lv_obj_create(s_cc);
    lv_obj_remove_style_all(s_cc_notes);
    lv_obj_set_flex_flow(s_cc_notes, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_cc_notes, 3, 0);
    lv_obj_set_scrollbar_mode(s_cc_notes, LV_SCROLLBAR_MODE_OFF);
    s_cc_clear = lv_label_create(s_cc);
    lv_label_set_text(s_cc_clear, "Clear all");
    lv_obj_set_style_text_font(s_cc_clear, &lv_font_montserrat_12, 0);
    purr_theme_apply(s_cc_clear, PURR_TH_ACCENT_TEXT);
    lv_obj_add_flag(s_cc_clear, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_cc_clear, clear_notes, LV_EVENT_CLICKED, NULL);
    s_cc_empty = lv_label_create(s_cc);
    lv_label_set_text(s_cc_empty, "No new notifications");
    lv_obj_set_style_text_font(s_cc_empty, &lv_font_montserrat_12, 0);
    purr_theme_apply(s_cc_empty, PURR_TH_DIM);

    s_cc_handle = lv_obj_create(s_cc);
    lv_obj_remove_style_all(s_cc_handle);
    lv_obj_add_flag(s_cc_handle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *bar = lv_obj_create(s_cc_handle);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 40, 3);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x808080), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_cc_handle, cc_drag_cb, LV_EVENT_ALL, NULL);

    /* the status strip is the other grab area; the sheet itself is created after the strip, so
     * the strip's own children still draw above it (build_status runs after this) */
    lv_obj_add_event_cb(s_status, cc_drag_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_flag(s_status, LV_OBJ_FLAG_CLICKABLE);

    cc_layout();
    add_note("PURR OS", "Control center ready. Pull down from the top edge, or tap the clock.");
}

/* ---------------------------------------------------------------- the app list
 *
 * All apps, alphabetical with a letter heading for each run, reached from the round button at the
 * end of the start screen. It slides in from the right over the start screen and back out with
 * the arrow in its header. */

static lv_obj_t *s_list;                /* the whole view */
static lv_obj_t *s_list_head;
static lv_obj_t *s_list_body;           /* the scrolling rows */
static int s_list_open;

static int ci_cmp(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int x = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int y = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (x != y) {
            return x - y;
        }
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static void list_row_clicked(lv_event_t *e)
{
    const tile_def_t *d = lv_event_get_user_data(e);
    ESP_LOGI(TAG, "app list: '%s' tapped", d->name);
    open_def(d);
    list_move(0);
}

static void list_x_cb(void *obj, int32_t v) { lv_obj_set_x((lv_obj_t *)obj, v); }

static void list_anim_done(lv_anim_t *a)
{
    (void)a;
    if (!s_list_open) {
        lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);
    }
}

static void list_move(int open)
{
    s_list_open = open;
    if (open) {
        lv_obj_remove_flag(s_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_scroll_to(s_list_body, 0, 0, LV_ANIM_OFF);
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_list);
    lv_anim_set_exec_cb(&a, list_x_cb);
    lv_anim_set_values(&a, lv_obj_get_x(s_list), open ? 0 : disp_w());
    lv_anim_set_duration(&a, 200);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, list_anim_done);
    lv_anim_start(&a);
}

static void list_back_clicked(lv_event_t *e)
{
    (void)e;
    list_move(0);
}

static void allapps_clicked(lv_event_t *e)
{
    (void)e;
    list_move(1);
}

static void fill_list(void);

static void build_list(void)
{
    s_list = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_list);
    purr_theme_apply(s_list, PURR_TH_SHEET);
    lv_obj_clear_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);

    s_list_head = lv_obj_create(s_list);
    lv_obj_remove_style_all(s_list_head);
    lv_obj_set_height(s_list_head, 30);
    lv_obj_add_flag(s_list_head, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_list_head, list_back_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *arrow = lv_label_create(s_list_head);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT);
    purr_theme_apply(arrow, PURR_TH_TEXT);
    lv_obj_align(arrow, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_t *title = lv_label_create(s_list_head);
    lv_label_set_text(title, "Apps");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    purr_theme_apply(title, PURR_TH_TEXT);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 34, 0);

    s_list_body = lv_obj_create(s_list);
    lv_obj_remove_style_all(s_list_body);
    lv_obj_set_flex_flow(s_list_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list_body, 2, 0);
    lv_obj_set_style_pad_hor(s_list_body, 8, 0);
    lv_obj_set_scrollbar_mode(s_list_body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_list_body, LV_DIR_VER);

    fill_list();
    list_layout();
}

/* The rows, alphabetical, with a letter heading for each run. Run again when the apps change. */
static void fill_list(void)
{
    lv_obj_clean(s_list_body);
    int order[MAX_TILES];
    int n = 0;
    for (int i = 0; i < N_TILE_DEFS && n < MAX_TILES; i++) {
        order[n++] = i;
    }
    for (int i = 1; i < n; i++) {                   /* a small insertion sort: a dozen names */
        int k = order[i], j = i - 1;
        while (j >= 0 && ci_cmp(s_tile_defs[order[j]].name, s_tile_defs[k].name) > 0) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = k;
    }
    char last = 0;
    for (int i = 0; i < n; i++) {
        const tile_def_t *d = &s_tile_defs[order[i]];
        char letter = d->name[0];
        if (letter >= 'a' && letter <= 'z') {
            letter = (char)(letter - 32);
        }
        if (letter != last) {
            last = letter;
            lv_obj_t *h = lv_label_create(s_list_body);
            char txt[2] = {letter, 0};
            lv_label_set_text(h, txt);
            lv_obj_set_style_text_font(h, &lv_font_montserrat_16, 0);
            purr_theme_apply(h, PURR_TH_ACCENT_TEXT);
            lv_obj_set_style_pad_top(h, 4, 0);
        }
        lv_obj_t *row = lv_obj_create(s_list_body);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), 34);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        purr_theme_apply(row, PURR_TH_ROW_PRESSED);
        lv_obj_add_event_cb(row, list_row_clicked, LV_EVENT_CLICKED, (void *)d);
        lv_obj_t *chip = lv_obj_create(row);
        lv_obj_remove_style_all(chip);
        lv_obj_set_size(chip, 30, 30);
        purr_theme_apply(chip, PURR_TH_ACCENT_BG);
        lv_obj_align(chip, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_remove_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        const lv_image_dsc_t *ic = d->icon ? img(d->icon) : NULL;
        if (ic != NULL) {
            lv_obj_t *im = lv_image_create(chip);
            lv_image_set_src(im, ic);
            lv_image_set_scale(im, 224);                 /* the icons are drawn a little large */
            lv_obj_align(im, LV_ALIGN_CENTER, 0, 0);
        }
        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, d->name);
        purr_theme_apply(nm, PURR_TH_TEXT);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 40, 0);
    }
}

static void list_layout(void)
{
    int w = disp_w(), h = disp_h();
    lv_obj_set_size(s_list, w, h - STATUS_H);
    lv_obj_set_pos(s_list, s_list_open ? 0 : w, STATUS_H);
    lv_obj_set_size(s_list_head, w, 30);
    lv_obj_align(s_list_head, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_size(s_list_body, w, h - STATUS_H - 30);
    lv_obj_align(s_list_body, LV_ALIGN_TOP_LEFT, 0, 30);
    if (!s_list_open) {
        lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---------------------------------------------------------------- shared by the two sign-in screens */

static int s_signed_in;                 /* someone has signed in since boot: the BFU screen is done */
static lv_obj_t *wp_image(lv_obj_t *parent, const char *asset)
{
    lv_obj_t *w = lv_image_create(parent);
    const lv_image_dsc_t *d = img(asset);
    if (d != NULL) {
        lv_image_set_src(w, d);
        lv_image_set_inner_align(w, LV_IMAGE_ALIGN_CENTER);
    }
    lv_obj_remove_flag(w, LV_OBJ_FLAG_CLICKABLE);
    return w;
}

/* ---------------------------------------------------------------- the lock screen
 *
 * The Windows 10 Mobile lock screen, used from the second unlock on: the wallpaper, the time and
 * date, and "swipe up to unlock". It is only a lock, not a sign-in: the first unlock of a boot is
 * the sign-in screen below, and what is on the device stays signed in behind this. */

static lv_obj_t *s_lock;
static lv_obj_t *s_lock_wall, *s_lock_time, *s_lock_date, *s_lock_hint;
static int s_lock_on;
static int s_lock_y0, s_lock_drag;

static void lock_y_cb(void *obj, int32_t v) { lv_obj_set_y((lv_obj_t *)obj, v); }

static void lock_anim_done(lv_anim_t *a)
{
    (void)a;
    if (!s_lock_on) {
        lv_obj_add_flag(s_lock, LV_OBJ_FLAG_HIDDEN);
    }
}

static void lock_anim(int to, int ms)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_lock);
    lv_anim_set_exec_cb(&a, lock_y_cb);
    lv_anim_set_values(&a, lv_obj_get_y(s_lock), to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, lock_anim_done);
    lv_anim_start(&a);
}

static void lock_clock_refresh(void)
{
    if (s_lock == NULL) {
        return;
    }
    time_t now = time(NULL);
    if (now > 1000000000) {
        struct tm tm;
        localtime_r(&now, &tm);
        char tb[12], db[32];
        fmt_hm(&tm, tb, sizeof(tb));
        strftime(db, sizeof(db), "%A, %d %B", &tm);
        lv_label_set_text(s_lock_time, tb);
        lv_label_set_text(s_lock_date, db);
    } else {
        lv_label_set_text(s_lock_time, "--:--");
        lv_label_set_text(s_lock_date, "Clock not set");
    }
}

static void lock_drag_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    if (in == NULL) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(in, &p);
    int h = disp_h();
    if (code == LV_EVENT_PRESSED) {
        lv_anim_delete(s_lock, lock_y_cb);
        s_lock_y0 = p.y;
        s_lock_drag = 0;
    } else if (code == LV_EVENT_PRESSING) {
        int dy = p.y - s_lock_y0;
        if (dy < -4) {
            s_lock_drag = 1;
        }
        if (s_lock_drag) {
            lv_obj_set_y(s_lock, dy < 0 ? dy : 0);       /* follows the finger, upwards only */
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (s_lock_drag && lv_obj_get_y(s_lock) < -h / 4) {
            s_lock_on = 0;
            lock_anim(-h, 200);                          /* off the top, and the start screen is there */
        } else if (s_lock_drag) {
            lock_anim(0, 180);                           /* not far enough: falls back */
        } else if (code == LV_EVENT_RELEASED) {
            lock_anim(-18, 90);                          /* a tap only nudges: it needs a swipe */
        }
    }
}

static void lock_nudge_back_cb(lv_timer_t *t)
{
    if (s_lock_on && lv_obj_get_y(s_lock) < 0 && lv_obj_get_y(s_lock) > -30) {
        lock_anim(0, 120);
    }
    (void)t;
}

static void build_lock(void)
{
    s_lock = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_lock);
    lv_obj_set_style_bg_color(s_lock, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_lock, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_lock, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_lock, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_lock, LV_OBJ_FLAG_HIDDEN);
    s_lock_wall = wp_image(s_lock, "img1");
    lv_obj_t *veil = lv_obj_create(s_lock);
    lv_obj_remove_style_all(veil);
    lv_obj_set_size(veil, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(veil, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(veil, LV_OPA_30, 0);
    lv_obj_remove_flag(veil, LV_OBJ_FLAG_CLICKABLE);

    s_lock_time = lv_label_create(s_lock);
    lv_obj_set_style_text_font(s_lock_time, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(s_lock_time, lv_color_white(), 0);
    s_lock_date = lv_label_create(s_lock);
    lv_obj_set_style_text_font(s_lock_date, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_lock_date, lv_color_white(), 0);
    s_lock_hint = lv_label_create(s_lock);
    lv_label_set_text(s_lock_hint, LV_SYMBOL_UP "  Swipe up to unlock");
    lv_obj_set_style_text_font(s_lock_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lock_hint, lv_color_hex(0xDDDDDD), 0);

    lv_obj_add_event_cb(s_lock, lock_drag_cb, LV_EVENT_ALL, NULL);
    lv_timer_create(lock_nudge_back_cb, 400, NULL);
    lock_layout();
}

static void lock_layout(void)
{
    if (s_lock == NULL) {
        return;
    }
    int w = disp_w(), h = disp_h();
    lv_obj_set_size(s_lock, w, h);
    lv_obj_set_pos(s_lock, 0, s_lock_on ? 0 : -h);
    lv_obj_set_size(s_lock_wall, w, h);
    lv_obj_align(s_lock_wall, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_align(s_lock_time, LV_ALIGN_BOTTOM_LEFT, 14, -52);
    lv_obj_align(s_lock_date, LV_ALIGN_BOTTOM_LEFT, 16, -30);
    lv_obj_align(s_lock_hint, LV_ALIGN_BOTTOM_MID, 0, -6);
    if (!s_lock_on) {
        lv_obj_add_flag(s_lock, LV_OBJ_FLAG_HIDDEN);
    }
    lock_clock_refresh();
}

static void lock_show(void)
{
    s_lock_on = 1;
    lock_clock_refresh();
    lv_obj_remove_flag(s_lock, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(s_lock, 0);
    if (s_cc_open) {
        cc_move(0);
    }
    if (s_list_open) {
        list_move(0);
    }
}

/* ---------------------------------------------------------------- the sign-in screen (BFU)
 *
 * Before the first unlock of a boot, the Windows 10 sign-in screen: a round avatar, the title, a
 * user name field and a password field with an arrow. Nothing else is reachable until it succeeds,
 * and nothing is started for a user before then (the runtime's exec-once apps wait for it too).
 * It is the same screen every time, "Other user": a name and a password. */

static lv_obj_t *s_bfu;
static lv_obj_t *s_bfu_wall, *s_bfu_avatar, *s_bfu_title, *s_bfu_user, *s_bfu_pass, *s_bfu_go,
                *s_bfu_msg;
static lv_group_t *s_bfu_group;
static int s_bfu_busy;

typedef struct {
    char name[PURR_USER_NAME_LEN];
    char pass[64];
    purr_login_result_t result;
    uint32_t wait_s;
} signin_job_t;

static void bfu_set_busy(int busy)
{
    s_bfu_busy = busy;
    if (busy) {
        lv_obj_add_state(s_bfu_user, LV_STATE_DISABLED);
        lv_obj_add_state(s_bfu_pass, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(s_bfu_user, LV_STATE_DISABLED);
        lv_obj_remove_state(s_bfu_pass, LV_STATE_DISABLED);
    }
}

static void bfu_say(const char *text, uint32_t color)
{
    lv_label_set_text(s_bfu_msg, text);
    lv_obj_set_style_text_color(s_bfu_msg, lv_color_hex(color), 0);
}

static void *signin_done_cb(void *arg)
{
    signin_job_t *j = arg;
    bfu_set_busy(0);
    if (j->result == PURR_LOGIN_OK) {
        s_signed_in = 1;
        lv_textarea_set_text(s_bfu_user, "");
        lv_textarea_set_text(s_bfu_pass, "");
        lv_obj_add_flag(s_bfu, LV_OBJ_FLAG_HIDDEN);
        lv_indev_t *kb = purr_lvgl_keypad();
        if (kb != NULL) {
            lv_indev_set_group(kb, NULL);                 /* the keyboard is free for apps again */
        }
        ESP_LOGI(TAG, "signed in as %s", j->name);
        lv_label_set_text(s_status_user, j->name);
        purr_theme_load_user(j->name);
        purr_session_started();
        apps_refresh();
        char msg[64];
        snprintf(msg, sizeof(msg), "Signed in as %s.", j->name);
        add_note("PURR OS", msg);
    } else if (j->result == PURR_LOGIN_WAIT) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Too many tries. Wait %u s.", (unsigned)j->wait_s);
        bfu_say(msg, 0xFFB900);
        lv_textarea_set_text(s_bfu_pass, "");
    } else {
        bfu_say("The user name or password is incorrect.", 0xFFB900);
        lv_textarea_set_text(s_bfu_pass, "");
        lv_group_focus_obj(s_bfu_pass);
    }
    memset(j, 0, sizeof(*j));
    free(j);
    return NULL;
}

/* The password check is a long PBKDF2 hash, so it runs on its own task and the screen stays live. */
static void signin_task(void *arg)
{
    signin_job_t *j = arg;
    j->result = purr_login_try(j->name, j->pass, &j->wait_s);
    memset(j->pass, 0, sizeof(j->pass));
    purr_ui_service_run(signin_done_cb, j, NULL);        /* frees j on the UI task */
    vTaskDelete(NULL);
}

static void bfu_submit(void)
{
    if (s_bfu_busy) {
        return;
    }
    const char *name = lv_textarea_get_text(s_bfu_user);
    const char *pass = lv_textarea_get_text(s_bfu_pass);
    if (name[0] == '\0') {
        lv_group_focus_obj(s_bfu_user);
        return;
    }
    signin_job_t *j = calloc(1, sizeof(*j));
    if (j == NULL) {
        bfu_say("Out of memory.", 0xFFB900);
        return;
    }
    snprintf(j->name, sizeof(j->name), "%s", name);
    snprintf(j->pass, sizeof(j->pass), "%s", pass);
    bfu_set_busy(1);
    bfu_say("Signing in...", 0xFFFFFF);
    if (xTaskCreate(signin_task, "signin", 6144, j, 4, NULL) != pdPASS) {
        bfu_set_busy(0);
        bfu_say("Could not start the sign-in.", 0xFFB900);
        free(j);
    }
}

static void bfu_user_ready(lv_event_t *e)                 /* Enter in the name field */
{
    (void)e;
    lv_group_focus_obj(s_bfu_pass);
}

static void bfu_pass_ready(lv_event_t *e)                 /* Enter in the password field */
{
    (void)e;
    bfu_submit();
}

static void bfu_go_clicked(lv_event_t *e)
{
    (void)e;
    bfu_submit();
}

static void bfu_avatar_draw(lv_obj_t *circle)
{
    /* a person drawn with two shapes: a ring for the head and an arc for the shoulders */
    lv_obj_t *head = lv_obj_create(circle);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, 26, 26);
    lv_obj_set_style_radius(head, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(head, 3, 0);
    lv_obj_set_style_border_color(head, lv_color_white(), 0);
    lv_obj_align(head, LV_ALIGN_CENTER, 0, -12);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *body = lv_arc_create(circle);
    lv_obj_set_size(body, 52, 52);
    lv_arc_set_bg_angles(body, 205, 335);
    lv_obj_set_style_arc_width(body, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(body, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(body, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_remove_style(body, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(body, LV_ALIGN_CENTER, 0, 22);
}

static lv_obj_t *bfu_field(lv_obj_t *parent, const char *placeholder, int password, int light)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_textarea_set_password_mode(ta, password);
    lv_textarea_set_max_length(ta, password ? 63 : PURR_USER_NAME_LEN - 1);
    lv_obj_set_style_radius(ta, 0, 0);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_all(ta, 4, 0);
    lv_obj_set_scrollbar_mode(ta, LV_SCROLLBAR_MODE_OFF);
    if (light) {                                        /* the user name: white, dark text */
        lv_obj_set_style_bg_color(ta, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(ta, 1, 0);
        lv_obj_set_style_border_color(ta, lv_color_hex(0xAAAAAA), 0);
        lv_obj_set_style_text_color(ta, lv_color_black(), 0);
        lv_obj_set_style_text_color(ta, lv_color_hex(0x707070), LV_PART_TEXTAREA_PLACEHOLDER);
    } else {                                            /* the password: dark and translucent */
        lv_obj_set_style_bg_color(ta, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(ta, LV_OPA_50, 0);
        lv_obj_set_style_border_width(ta, 1, 0);
        lv_obj_set_style_border_color(ta, lv_color_hex(0xDDDDDD), 0);
        lv_obj_set_style_text_color(ta, lv_color_white(), 0);
        lv_obj_set_style_text_color(ta, lv_color_hex(0xC8C8C8), LV_PART_TEXTAREA_PLACEHOLDER);
    }
    return ta;
}

static void build_bfu(void)
{
    s_bfu = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_bfu);
    lv_obj_set_style_bg_color(s_bfu, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_bfu, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_bfu, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_bfu, LV_OBJ_FLAG_CLICKABLE);      /* nothing beneath it can be touched */

    s_bfu_wall = wp_image(s_bfu, "img3");
    lv_obj_t *veil = lv_obj_create(s_bfu);
    lv_obj_remove_style_all(veil);
    lv_obj_set_size(veil, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(veil, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(veil, LV_OPA_20, 0);
    lv_obj_remove_flag(veil, LV_OBJ_FLAG_CLICKABLE);

    s_bfu_avatar = lv_obj_create(s_bfu);
    lv_obj_remove_style_all(s_bfu_avatar);
    lv_obj_set_style_radius(s_bfu_avatar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_bfu_avatar, lv_color_hex(0x6B6B6B), 0);
    lv_obj_set_style_bg_opa(s_bfu_avatar, LV_OPA_COVER, 0);
    lv_obj_set_style_clip_corner(s_bfu_avatar, true, 0);
    lv_obj_clear_flag(s_bfu_avatar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_bfu_avatar, LV_OBJ_FLAG_CLICKABLE);
    bfu_avatar_draw(s_bfu_avatar);

    s_bfu_title = lv_label_create(s_bfu);
    lv_label_set_text(s_bfu_title, "Other user");
    lv_obj_set_style_text_font(s_bfu_title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_bfu_title, lv_color_white(), 0);

    s_bfu_user = bfu_field(s_bfu, "User name", 0, 1);
    s_bfu_pass = bfu_field(s_bfu, "Password", 1, 0);
    lv_obj_add_event_cb(s_bfu_user, bfu_user_ready, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_bfu_pass, bfu_pass_ready, LV_EVENT_READY, NULL);

    s_bfu_go = lv_obj_create(s_bfu);                    /* the arrow, at the right of the password */
    lv_obj_remove_style_all(s_bfu_go);
    lv_obj_set_style_bg_color(s_bfu_go, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_bfu_go, LV_OPA_30, 0);
    lv_obj_set_style_bg_opa(s_bfu_go, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_add_flag(s_bfu_go, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_bfu_go, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_bfu_go, bfu_go_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ar = lv_label_create(s_bfu_go);
    lv_label_set_text(ar, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(ar, lv_color_white(), 0);
    lv_obj_align(ar, LV_ALIGN_CENTER, 0, 0);

    s_bfu_msg = lv_label_create(s_bfu);
    lv_label_set_text(s_bfu_msg, "");
    lv_obj_set_style_text_font(s_bfu_msg, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(s_bfu_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_bfu_msg, LV_TEXT_ALIGN_CENTER, 0);

    /* the hardware keyboard types into whichever field has focus; Tab moves between them */
    s_bfu_group = lv_group_create();
    lv_group_add_obj(s_bfu_group, s_bfu_user);
    lv_group_add_obj(s_bfu_group, s_bfu_pass);
    lv_indev_t *kb = purr_lvgl_keypad();
    if (kb != NULL) {
        lv_indev_set_group(kb, s_bfu_group);
    }
    lv_group_focus_obj(s_bfu_user);
    bfu_layout();
}

static void bfu_layout(void)
{
    if (s_bfu == NULL) {
        return;
    }
    int w = disp_w(), h = disp_h();
    int land = is_landscape();
    int av = 72;                                        /* the avatar's diameter (the person is drawn for it) */
    int fw = land ? 190 : 196;                          /* the fields' width */
    lv_obj_set_size(s_bfu, w, h);
    lv_obj_set_pos(s_bfu, 0, 0);
    lv_obj_set_size(s_bfu_wall, w, h);
    lv_obj_align(s_bfu_wall, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_size(s_bfu_avatar, av, av);
    int top = land ? 6 : 44;
    lv_obj_align(s_bfu_avatar, LV_ALIGN_TOP_MID, 0, top);
    lv_obj_align(s_bfu_title, LV_ALIGN_TOP_MID, 0, top + av + 2);
    int fy = top + av + 2 + 36;
    lv_obj_set_size(s_bfu_user, fw, 26);
    lv_obj_align(s_bfu_user, LV_ALIGN_TOP_MID, 0, fy);
    lv_obj_set_size(s_bfu_pass, fw - 26, 26);
    lv_obj_align(s_bfu_pass, LV_ALIGN_TOP_MID, -13, fy + 30);
    lv_obj_set_size(s_bfu_go, 26, 26);
    lv_obj_align_to(s_bfu_go, s_bfu_pass, LV_ALIGN_OUT_RIGHT_MID, 0, 0);
    lv_obj_set_width(s_bfu_msg, fw);
    lv_obj_align(s_bfu_msg, LV_ALIGN_TOP_MID, 0, fy + 62);
}

/* ---------------------------------------------------------------- the start screen */

static void size_for_orientation(void)
{
    int w = disp_w(), h = disp_h();
    lv_obj_set_size(s_wall, w, h);
    lv_obj_align(s_wall, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_size(s_shade, w, h);
    lv_obj_align(s_shade, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_size(s_start, w, h - STATUS_H);
    lv_obj_align(s_start, LV_ALIGN_TOP_LEFT, 0, STATUS_H);
    lv_obj_set_scroll_dir(s_start, is_landscape() ? LV_DIR_HOR : LV_DIR_VER);
}

static void relayout(void)
{
    size_for_orientation();
    pack_tiles();
    position_tiles();
    lv_obj_scroll_to(s_start, 0, 0, LV_ANIM_OFF);
    if (s_cc != NULL) {
        cc_layout();
    }
    if (s_list != NULL) {
        list_layout();
    }
    w10m_settings_layout();
    w10m_devportal_layout();
    purr_catrt_ui_layout();
    lock_layout();
    bfu_layout();
}

/* The shell's Settings, then the installed apps that verify (an unsigned app is never listed, so it
 * can never be started from here either). */
static void load_defs(void)
{
    static purr_app_registry_t reg;                  /* ~1.6 KB: not for the UI task's stack */
    s_ndefs = 0;
    tile_def_t *d = &s_tile_defs[s_ndefs++];
    memset(d, 0, sizeof(*d));
    snprintf(d->name, sizeof(d->name), "Settings");
    d->icon = "wp_settings";
    d->system = 1;

    memset(&reg, 0, sizeof(reg));
    if (purr_login_current()->name[0] != '\0') {
        purr_apps_scan_current_user(&reg);
    }
    for (int i = 0; i < reg.count && s_ndefs < MAX_TILES; i++) {
        const purr_app_entry_t *a = &reg.apps[i];
        /* an unsigned app is listed only while developer mode allows unsigned apps (they are in this user's
         * own folder, which is all the scan looks at) */
        if (!a->chip_ok || !(a->verified || purr_security_unsigned_apps_allowed()) ||
            strcmp(a->name, "Settings") == 0) {
            continue;
        }
        d = &s_tile_defs[s_ndefs++];
        memset(d, 0, sizeof(*d));
        snprintf(d->name, sizeof(d->name), "%s", a->name);
        d->dev = !a->verified;
    }

    /* Developer mode: the Developer Portal is an app of its own on the Home screen, and what it installed
     * (the devapps folder) is listed with a DEV mark. A developer app wins over an installed one of the
     * same name, as it does when it is opened. */
    if (purr_security_dev_active() && purr_login_current()->name[0] != '\0' && s_ndefs < MAX_TILES) {
        d = &s_tile_defs[s_ndefs++];
        memset(d, 0, sizeof(*d));
        snprintf(d->name, sizeof(d->name), "Developer Portal");
        d->system = 2;
        d->dev = 1;
        memset(&reg, 0, sizeof(reg));
        purr_devapps_scan(&reg);
        for (int i = 0; i < reg.count; i++) {
            const purr_app_entry_t *a = &reg.apps[i];
            if (!a->chip_ok || !(a->verified || purr_security_unsigned_apps_allowed())) {
                continue;
            }
            tile_def_t *slot = NULL;
            for (int k = 0; k < s_ndefs; k++) {
                if (strcmp(s_tile_defs[k].name, a->name) == 0) {
                    slot = &s_tile_defs[k];
                }
            }
            if (slot == NULL && s_ndefs < MAX_TILES) {
                slot = &s_tile_defs[s_ndefs++];
            }
            if (slot != NULL) {
                memset(slot, 0, sizeof(*slot));
                snprintf(slot->name, sizeof(slot->name), "%s", a->name);
                slot->dev = 1;
            }
        }
    }
}

static void make_tiles(void)
{
    s_ntiles = 0;
    for (int i = 0; i < s_ndefs && i < MAX_TILES; i++) {
        tile_t *t = &s_tiles[s_ntiles++];
        memset(t, 0, sizeof(*t));
        t->def = &s_tile_defs[i];
        t->obj = make_tile(s_start, t);
    }
}

/* Re-reads the installed apps and rebuilds the tiles and the app list. UI task only. */
static void apps_refresh(void)
{
    for (int i = 0; i < s_ntiles; i++) {
        lv_obj_delete(s_tiles[i].obj);
    }
    load_defs();
    make_tiles();
    if (s_list_body != NULL) {
        fill_list();
    }
    relayout();
}

static void *build_cb(void *arg)
{
    (void)arg;
    purr_theme_init();
    int saved_b = purr_settings_kv_int("display.brightness", 255);
    w10m_set_brightness(saved_b < 1 ? 255 : saved_b);
    int saved_r = purr_settings_kv_int("display.rotation", -1);
    if (saved_r >= 0 && saved_r <= 270 && saved_r % 90 == 0) {
        purr_lvgl_set_rotation(saved_r);            /* before anything is laid out */
    }
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_wall = lv_image_create(scr);
    const lv_image_dsc_t *wp = img(s_wallpaper);
    if (wp != NULL) {
        lv_image_set_src(s_wall, wp);
        lv_image_set_inner_align(s_wall, LV_IMAGE_ALIGN_CENTER);   /* cropped to the screen, not scaled */
    }
    lv_obj_remove_flag(s_wall, LV_OBJ_FLAG_CLICKABLE);

    s_shade = lv_obj_create(scr);
    lv_obj_remove_style_all(s_shade);
    lv_obj_set_style_bg_color(s_shade, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_shade, LV_OPA_60, 0);
    lv_obj_remove_flag(s_shade, LV_OBJ_FLAG_CLICKABLE);

    s_start = lv_obj_create(scr);
    lv_obj_remove_style_all(s_start);
    lv_obj_set_scrollbar_mode(s_start, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(s_start, 0, 0);

    s_ntiles = 0;
    load_defs();
    make_tiles();

    /* the all-apps button, after the tiles */
    s_allapps = lv_obj_create(s_start);
    lv_obj_remove_style_all(s_allapps);
    lv_obj_clear_flag(s_allapps, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_allapps, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_allapps, allapps_clicked, LV_EVENT_CLICKED, NULL);
    s_allapps_circle = lv_obj_create(s_allapps);
    lv_obj_remove_style_all(s_allapps_circle);
    lv_obj_set_size(s_allapps_circle, 40, 40);
    lv_obj_set_style_radius(s_allapps_circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_allapps_circle, 2, 0);
    lv_obj_set_style_border_color(s_allapps_circle, lv_color_white(), 0);
    lv_obj_set_style_bg_color(s_allapps_circle, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_remove_flag(s_allapps_circle, LV_OBJ_FLAG_CLICKABLE);
    s_allapps_arrow = lv_label_create(s_allapps_circle);
    lv_label_set_text(s_allapps_arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(s_allapps_arrow, lv_color_white(), 0);
    lv_obj_align(s_allapps_arrow, LV_ALIGN_CENTER, 0, 0);
    s_allapps_label = lv_label_create(s_allapps);
    lv_label_set_text(s_allapps_label, "All apps");
    lv_obj_set_style_text_font(s_allapps_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_allapps_label, lv_color_white(), 0);

    s_signed_in = purr_login_current()->name[0] != '\0';   /* a session already exists (first-time setup) */

    build_list();
    w10m_settings_build();
    w10m_devportal_build();
    build_lock();                       /* beneath the status strip, which stays visible over it */
    build_status();
    build_cc();
    build_bfu();                        /* last, so it covers everything until someone signs in */
    purr_theme_on_change(theme_changed);
    if (purr_login_autologin_used()) {
        char msg[80];
        snprintf(msg, sizeof(msg), "Signed in automatically as %s (developer mode).", purr_login_current()->name);
        add_note("PURR OS", msg);
    }
    if (s_signed_in) {
        purr_theme_load_user(purr_login_current()->name);
        lv_obj_add_flag(s_bfu, LV_OBJ_FLAG_HIDDEN);
        lv_indev_t *kb = purr_lvgl_keypad();
        if (kb != NULL) {
            lv_indev_set_group(kb, NULL);
        }
    }
    relayout();
    ESP_LOGI(TAG, "shell up: %dx%d, %d tiles, accent %s", disp_w(), disp_h(), s_ntiles,
             purr_theme_accent_at(purr_theme_accent())->name);
    return NULL;
}

#if CONFIG_PURR_W10M_ROTATE_DEMO
static void rotate_demo(void *arg)
{
    (void)arg;
    int deg = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(9000));
        deg = (deg + 90) % 360;
        purr_w10m_set_orientation(deg);
    }
}
#endif

/* ---------------------------------------------------------------- settings by key (display, shell) */

static int bright_get(char *o, size_t c) { snprintf(o, c, "%d", s_brightness); return 0; }

static int bright_set(const char *v, char *e, size_t c)
{
    int n = atoi(v);
    if (n < 1 || n > 255) {
        snprintf(e, c, "expected 1-255");
        return -1;
    }
    w10m_set_brightness(n);
    w10m_save_display();
    return 0;
}

static int rot_get(char *o, size_t c) { snprintf(o, c, "%d", purr_lvgl_get_rotation()); return 0; }

static int rot_set(const char *v, char *e, size_t c)
{
    int n = atoi(v);
    if (n < 0 || n > 270 || n % 90 != 0) {
        snprintf(e, c, "expected 0, 90, 180 or 270");
        return -1;
    }
    if (purr_w10m_set_orientation(n) != ESP_OK) {
        snprintf(e, c, "the screen is not running");
        return -1;
    }
    return 0;
}

static int shell_get(char *o, size_t c)
{
    purr_ui_mode_t m = purr_ui_mode_get();
    snprintf(o, c, "%s", m == PURR_UI_MODE_W10M ? "w10m" : m == PURR_UI_MODE_NAV ? "nav" : "card_shell");
    return 0;
}

static int shell_set(const char *v, char *e, size_t c)
{
    if (strcmp(v, "w10m") != 0 && strcmp(v, "nav") != 0 && strcmp(v, "card_shell") != 0) {
        snprintf(e, c, "expected w10m, nav or card_shell");
        return -1;
    }
    purr_fs_t *fs = purr_login_fs();
    char line[24];
    snprintf(line, sizeof(line), "%s\n", v);
    purr_fs_mkdir(fs, "/ui");
    if (purr_fs_write(fs, "/ui/mode.conf", line, (uint32_t)strlen(line)) < 0) {
        snprintf(e, c, "could not save");
        return -1;
    }
    return 0;                                       /* takes effect at the next boot */
}

static const purr_setting_t s_w10m_keys[] = {
    {"display.brightness", "backlight", "1-255", 0, bright_get, bright_set},
    {"display.rotation", "screen orientation in degrees", "0|90|180|270", 0, rot_get, rot_set},
    {"ui.shell", "which shell starts (next boot)", "w10m|nav|card_shell", 1, shell_get, shell_set},
};

void w10m_place_sheet(lv_obj_t *sheet)
{
    /* The lock screen, sign-in, control center and status strip are on the top layer, which is always above the screen. The
     * sheets (Settings, the portal, an app window) are children of the screen, so the newest on top is all that is needed.
     * (This used to move the sheet to the lock screen's INDEX, which counts a different layer's children: it buried every app
     * window under the start screen.) */
    lv_obj_move_foreground(sheet);
}

esp_err_t purr_w10m_start(void)
{
    purr_catrt_ui_init();
    w10m_perm_init();
    purr_console_set_muted(1);                      /* the text console must not paint over the shell */
    for (size_t i = 0; i < sizeof(s_w10m_keys) / sizeof(s_w10m_keys[0]); i++) {
        purr_settings_register(&s_w10m_keys[i]);
    }
    esp_err_t e = purr_ui_service_run(build_cb, NULL, NULL);
#if CONFIG_PURR_W10M_ROTATE_DEMO
    if (e == ESP_OK) {
        xTaskCreate(rotate_demo, "w10m-rot", 3072, NULL, 3, NULL);
    }
#endif
    return e;
}

void w10m_apply_orientation(int degrees)
{
    if (purr_lvgl_set_rotation(degrees) == 0) {
        relayout();
        ESP_LOGI(TAG, "orientation %d: %dx%d", degrees, disp_w(), disp_h());
        char v[8];
        snprintf(v, sizeof(v), "%d", degrees);
        purr_settings_kv_set("display.rotation", v);
    }
}

static void *orient_cb(void *arg)
{
    int deg = (int)(intptr_t)arg;
    if (purr_lvgl_set_rotation(deg) != 0) {
        return (void *)(intptr_t)-1;
    }
    w10m_apply_orientation(deg);                    /* relayout, and remember it */
    return NULL;
}

esp_err_t purr_w10m_set_orientation(int degrees)
{
    void *r = NULL;
    esp_err_t e = purr_ui_service_run(orient_cb, (void *)(intptr_t)degrees, &r);
    if (e != ESP_OK) {
        return e;
    }
    return r == NULL ? ESP_OK : ESP_ERR_INVALID_ARG;
}

typedef struct {
    char title[40];
    char text[120];
} note_args_t;

void w10m_apps_changed(void)
{
    if (s_start != NULL) {
        apps_refresh();
    }
}

static void *refresh_cb(void *arg)
{
    (void)arg;
    if (s_start != NULL) {
        apps_refresh();
    }
    return NULL;
}

esp_err_t purr_w10m_refresh_apps(void)
{
    return purr_ui_service_run(refresh_cb, NULL, NULL);
}

static void *notify_cb(void *arg)
{
    const note_args_t *n = arg;
    if (s_cc_notes != NULL) {
        add_note(n->title, n->text);
    }
    return NULL;
}

esp_err_t purr_w10m_notify(const char *title, const char *text)
{
    note_args_t n;
    snprintf(n.title, sizeof(n.title), "%s", title ? title : "");
    snprintf(n.text, sizeof(n.text), "%s", text ? text : "");
    return purr_ui_service_run(notify_cb, &n, NULL);
}

static void *lock_cb(void *arg)
{
    (void)arg;
    if (s_lock != NULL && s_signed_in) {
        lock_show();
    }
    return NULL;
}

esp_err_t purr_w10m_lock(void)
{
    return purr_ui_service_run(lock_cb, NULL, NULL);
}
