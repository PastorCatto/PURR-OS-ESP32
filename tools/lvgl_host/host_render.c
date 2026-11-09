/* Renders the real OOBE pages (purr_oobe.c) with the real theme (purr_w10m_theme.c) and widgets (purr_w10m_widgets.c) on the
 * real LVGL, into PPM files. Fake platform operations stand in for Wi-Fi, the filesystem and the installer.
 *     host_render <out-prefix>      writes <prefix>NAME.ppm for every page */
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include <stdlib.h>
#include "purr_abi.h"
#include "purr_oobe.h"
#include "purr_w10m_theme.h"

#define W 320
#define H 240

static uint16_t fb[H][W];
static uint8_t buf[W * H * 2];
static const char *g_prefix = "";

void host_register_settings(void);

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    uint16_t *p = (uint16_t *)px;
    int w = a->x2 - a->x1 + 1;
    for (int y = a->y1; y <= a->y2; y++)
        for (int x = a->x1; x <= a->x2; x++)
            fb[y][x] = p[(y - a->y1) * w + (x - a->x1)];
    lv_display_flush_ready(d);
}

static void snap(const char *name)
{
    char path[256];
    snprintf(path, sizeof(path), "%s%s.ppm", g_prefix, name);
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint16_t v = fb[y][x];
            unsigned char px[3] = {(unsigned char)((v >> 11) * 255 / 31), (unsigned char)(((v >> 5) & 63) * 255 / 63),
                                   (unsigned char)((v & 31) * 255 / 31)};
            fwrite(px, 1, 3, f);
        }
    fclose(f);
}

static void run(int ms)
{
    for (int t = 0; t < ms; t += 20) {
        lv_tick_inc(20);
        lv_timer_handler();
    }
}

/* ---- fake platform */
static int g_polls, g_bg = -1;
static uint16_t g_reasons = PURR_RR_ALTERED | PURR_RR_REPEATED;
static uint16_t fake_reasons(void) { return g_reasons; }
static char *g_legal;
static const char *fake_legal(void)
{
    if (!g_legal) {
        FILE *f = fopen(LEGAL_PATH, "rb");
        if (f) { g_legal = calloc(1, 8192); fread(g_legal, 1, 8191, f); fclose(f); }
    }
    return g_legal;
}
static int g_devrec;
static int g_legal_fail;
static int fake_devrec(void) { return g_devrec; }
static int fake_servers(purr_oobe_server_t *out, int max)
{
    (void)max;
    snprintf(out[0].base, sizeof(out[0].base), "http://192.168.1.20:47801");
    snprintf(out[0].name, sizeof(out[0].name), "Claudes_Mac");
    snprintf(out[0].id, sizeof(out[0].id), "a1b2c3d4e5f6");
    return 1;
}
static void fake_use_server(const purr_oobe_server_t *s) { (void)s; }
static int fake_wired(void) { return 0; }
static const char *fake_remembered(void) { return "HomeWifi"; }
static int fake_job_start(purr_oobe_job_t k, const char *a, const char *b) { (void)k; (void)a; (void)b; g_polls = 0; return 0; }
static int fake_job_poll(purr_oobe_job_t k, char *msg, size_t cap, int *per)
{
    g_polls++;
    if (k == PURR_OOBE_JOB_DEVVERIFY) { if (g_polls > 3) snprintf(msg, cap, "Verified. It has PURR OS 1.0.0 for this device."); return g_polls > 3 ? 0 : -1; }
    if (k == PURR_OOBE_JOB_LEGAL && g_legal_fail) { if (g_polls > 3) { snprintf(msg, cap, "The server did not answer (legal.txt: ESP_ERR_HTTP_CONNECT)."); return 1; } return -1; }
    if (k == PURR_OOBE_JOB_RESTORE) { snprintf(msg, cap, "Downloading PURR OS"); *per = 620; return -1; }
    return g_polls > 3 ? 0 : -1;
}
static int fake_scan(purr_oobe_ap_t *out, int max)
{
    static const struct { const char *n; int r, s; } ap[] = {{"HomeWifi", -48, 1}, {"Workshop", -66, 1}, {"Guest", -79, 0}};
    int n = 0;
    for (; n < 3 && n < max; n++) { snprintf(out[n].ssid, sizeof(out[n].ssid), "%s", ap[n].n); out[n].rssi = ap[n].r; out[n].secure = ap[n].s; }
    return n;
}
static int fake_bg(char *msg, size_t cap) { if (g_bg >= 1000) snprintf(msg, cap, "System installed"); else if (g_bg >= 0) snprintf(msg, cap, "Installing the system..."); return g_bg; }
static void fake_finish(void) {}
static int fake_need(void) { return 1; }
static const purr_oobe_ops_t k_ops = {fake_reasons, fake_legal, fake_devrec, fake_servers, fake_use_server, fake_wired, fake_remembered, fake_job_start, fake_job_poll, fake_scan, fake_bg, fake_finish, NULL};

static lv_obj_t *find_ta(lv_obj_t *o, int *nth)
{
    if (lv_obj_check_type(o, &lv_textarea_class) && (*nth)-- == 0) return o;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        lv_obj_t *r = find_ta(lv_obj_get_child(o, i), nth);
        if (r) return r;
    }
    return NULL;
}

static void settext(int nth, const char *t)
{
    int n = nth;
    lv_obj_t *ta = find_ta(lv_screen_active(), &n);
    if (ta) lv_textarea_set_text(ta, t);
}

/* Click the visible button whose label reads `text`, as a finger would. */
static lv_obj_t *find_button(lv_obj_t *o, const char *text)
{
    if (lv_obj_check_type(o, &lv_button_class) && !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) && lv_obj_get_child_count(o) > 0) {
        lv_obj_t *l = lv_obj_get_child(o, 0);
        if (lv_obj_check_type(l, &lv_label_class) && !strcmp(lv_label_get_text(l), text)) return o;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        lv_obj_t *r = find_button(lv_obj_get_child(o, i), text);
        if (r) return r;
    }
    return NULL;
}

/* A tappable row (not a button): an object that is clickable and has a label child reading `text`. */
static lv_obj_t *find_row(lv_obj_t *o, const char *text)
{
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE) && !lv_obj_check_type(o, &lv_button_class) && lv_obj_get_child_count(o) > 0) {
        lv_obj_t *l = lv_obj_get_child(o, 0);
        if (lv_obj_check_type(l, &lv_label_class) && !strcmp(lv_label_get_text(l), text)) return o;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        lv_obj_t *r = find_row(lv_obj_get_child(o, i), text);
        if (r) return r;
    }
    return NULL;
}

static void press(const char *text)
{
    lv_obj_t *b = find_button(lv_screen_active(), text);
    if (!b) b = find_row(lv_screen_active(), text);
    if (b) lv_obj_send_event(b, LV_EVENT_CLICKED, NULL);
}

static void fresh(void)
{
    lv_obj_clean(lv_screen_active());
}

int main(int argc, char **argv)
{
    g_prefix = argc > 1 ? argv[1] : "";
    lv_init();
    lv_display_t *d = lv_display_create(W, H);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(d, flush_cb);
    lv_display_set_buffers(d, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_FULL);
    host_register_settings();
    purr_theme_init();

    char name[32];

    /* the first run of a blank device */
    g_reasons = PURR_RR_BLANK | PURR_RR_NO_SYSTEM;
    purr_oobe_start(PURR_OOBE_PART_LOADER, &k_ops, 0);
    run(400);
    snap("loader-first");
    fresh();
    g_reasons = PURR_RR_ALTERED | PURR_RR_REPEATED;

    /* the bootloader allowed a dev server: one extra page before the install */
    g_devrec = 1;
    purr_oobe_start(PURR_OOBE_PART_LOADER, &k_ops, 0);
    run(300);
    press("OK");
    run(500);
    press("HomeWifi");
    run(500);
    press("Next");
    run(900);
    snap("loader-dev-0");
    press("Claudes_Mac");
    run(700);
    snap("loader-dev-1");
    fresh();
    g_devrec = 0;

    /* part 1: the recovery loader. Each page is reached the way a person would: by pressing the buttons. */
    g_bg = -1;
    purr_oobe_start(PURR_OOBE_PART_LOADER, &k_ops, 0);
    run(400);
    snap("loader-0");
    press("OK");
    run(600);
    snap("loader-1");
    press("HomeWifi");              /* a remembered network: it connects without a password */
    run(600);
    press("Next");
    run(900);                       /* the legal text is "downloaded" (read from the file) */
    snap("loader-2");
    press("Continue");
    run(300);
    snap("loader-3");

    /* part 2: KittenOS first boot, with the install still running in the background */
    fresh();
    g_bg = 240;
    purr_oobe_start(PURR_OOBE_PART_FIRSTBOOT, &k_ops, 0);
    run(300);
    snap("first-0");
    press("Next");
    run(300);
    settext(0, "catto"); settext(1, "mypassword"); settext(2, "mypassword");
    run(200);
    g_bg = 480;
    snap("first-1");
    press("Next");                  /* creates the account (a job), then moves on */
    run(800);
    g_bg = 700;
    run(100);
    snap("first-2");
    press("Next");
    run(300);
    g_bg = 1000;
    run(700);
    { lv_obj_t *b = find_button(lv_screen_active(), "Finish"); printf("finish button disabled=%d\n", b ? lv_obj_has_state(b, LV_STATE_DISABLED) : -1); }
    snap("first-3");

#ifdef CONFIG_PURR_SHELL_ANDROID
    /* the Froyo skin's first run: six pages (the owner's draft), each opened directly, the install at three points of its progress */
    static const int bg_at[7] = {240, 240, 240, 240, 240, 620, 1000};
    for (int i = 0; i < 7; i++) {
        char name[16];
        fresh();
        g_bg = bg_at[i];
        purr_oobe_start(PURR_OOBE_PART_FIRSTBOOT, &k_ops, i);
        run(500);
        if (i == 2) { settext(0, "catto"); settext(1, "mypassword"); settext(2, "mypassword"); run(200); }
        snprintf(name, sizeof(name), "froyo-%d", i);
        snap(name);
    }
    fresh();
    g_bg = 300;
    purr_oobe_start(PURR_OOBE_PART_FIRSTBOOT, &k_ops, 5);
    run(300);
    snap("froyo-5-early");

    /* the legal page as the connection test: the text could not be loaded */
    g_legal_fail = 1;
    fresh();
    g_bg = 240;
    purr_oobe_start(PURR_OOBE_PART_FIRSTBOOT, &k_ops, 4);
    run(600);
    snap("froyo-legal-fail");
    g_legal_fail = 0;

    /* the install could not get online: the Wi-Fi page already in the list is the one that comes, not a second one (this was seven pages) */
    purr_oobe_ops_t k_need = k_ops;
    k_need.need_wifi = fake_need;
    fresh();
    g_bg = 240;
    purr_oobe_start(PURR_OOBE_PART_FIRSTBOOT, &k_need, 0);
    run(300);
    press("Next");
    run(400);
    snap("froyo-needwifi");
#endif
    return 0;
}
