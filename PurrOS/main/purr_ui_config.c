#include "sdkconfig.h"
#include "purr_ui_config.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "commands.h"
#include "purr_fs.h"
#include "purr_lvgl.h"
#include "purr_ui_service.h"

static const char *TAG = "ui_config";

/* ---------------------------------------------------------------- small text helpers */

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = '\0';
    return s;
}

/* Strips one pair of surrounding double quotes, if present. Mutates nothing if there are
 * none -- a bare word value (an identifier like an easing name) is left exactly as-is. */
static char *unquote(char *s)
{
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        s[n - 1] = '\0';
        return s + 1;
    }
    return s;
}

/* "#RRGGBB" -> 0x00RRGGBB. Returns 0 on success, -1 if it doesn't look like that. */
static int parse_color(const char *s, uint32_t *out)
{
    if (s[0] != '#' || strlen(s) != 7) {
        return -1;
    }
    char *end;
    unsigned long v = strtoul(s + 1, &end, 16);
    if (*end != '\0') {
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

/* Splits `s` (already past the '=') on commas, trimming and unquoting each piece. Mutates
 * `s` in place (commas become '\0'). Returns the count, capped at `max`. */
static int split_values(char *s, char *vals[], int max)
{
    int n = 0;
    char *p = s;
    while (n < max) {
        char *comma = strchr(p, ',');
        if (comma) *comma = '\0';
        vals[n++] = unquote(trim(p));
        if (!comma) break;
        p = comma + 1;
    }
    return n;
}

/* `word "name" {` -- the header line of a screen or widget block. `line` is already trimmed
 * and already confirmed to end with '{'. Returns 0 on success. */
static int parse_block_header(char *line, char *word, size_t word_cap,
                              char *name, size_t name_cap)
{
    size_t n = strlen(line);
    if (n == 0 || line[n - 1] != '{') {
        return -1;
    }
    line[n - 1] = '\0';
    trim(line);

    char *p = line;
    size_t wn = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '"' && wn + 1 < word_cap) {
        word[wn++] = *p++;
    }
    word[wn] = '\0';
    if (wn == 0) {
        return -1;
    }
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') {
        return -1;
    }
    p++;
    char *close = strchr(p, '"');
    if (!close) {
        return -1;
    }
    size_t nn = (size_t)(close - p);
    if (nn >= name_cap) nn = name_cap - 1;
    memcpy(name, p, nn);
    name[nn] = '\0';
    return 0;
}

/* `key = v1, v2, ...` -- a property line inside a block. Returns the value count (>=0), or
 * -1 if there's no '='. */
static int parse_property(char *line, char *key, size_t key_cap, char *vals[], int max_vals)
{
    char *eq = strchr(line, '=');
    if (!eq) {
        return -1;
    }
    *eq = '\0';
    char *k = trim(line);
    size_t kn = strlen(k);
    if (kn >= key_cap) kn = key_cap - 1;
    memcpy(key, k, kn);
    key[kn] = '\0';
    return split_values(trim(eq + 1), vals, max_vals);
}

/* ---------------------------------------------------------------- the parser */

typedef enum { ST_TOP, ST_SCREEN, ST_WIDGET } state_t;

esp_err_t purr_ui_config_parse(char *text, purr_ui_screen_t *out)
{
    memset(out, 0, sizeof(*out));
    out->background = PURR_UI_COLOR_UNSET;

    state_t state = ST_TOP;
    purr_ui_widget_t *cur = NULL;   /* NULL while inside an unrecognized/ignored widget block */
    int in_known_widget = 0;
    int line_no = 0;

    char *p = text;
    while (*p) {
        line_no++;
        char *line = p;
        char *nl = strchr(p, '\n');
        if (nl) { *nl = '\0'; p = nl + 1; } else { p += strlen(p); }
        size_t ll = strlen(line);
        if (ll > 0 && line[ll - 1] == '\r') line[--ll] = '\0';

        char *t = trim(line);
        if (*t == '\0' || *t == '#') {
            continue;                      /* blank line, or a whole-line comment */
        }

        if (strcmp(t, "}") == 0) {
            if (state == ST_WIDGET) {
                state = ST_SCREEN;
                cur = NULL;
            } else if (state == ST_SCREEN) {
                state = ST_TOP;
            } else {
                ESP_LOGW(TAG, "line %d: unexpected '}'", line_no);
                return ESP_ERR_INVALID_ARG;
            }
            continue;
        }

        if (t[strlen(t) - 1] == '{') {
            char word[PURR_UI_WORD_LEN], name[PURR_UI_NAME_LEN];
            if (parse_block_header(t, word, sizeof(word), name, sizeof(name)) != 0) {
                ESP_LOGW(TAG, "line %d: malformed block header", line_no);
                return ESP_ERR_INVALID_ARG;
            }
            if (state == ST_TOP) {
                if (strcmp(word, "screen") != 0) {
                    ESP_LOGW(TAG, "line %d: expected 'screen', got '%s'", line_no, word);
                    return ESP_ERR_INVALID_ARG;
                }
                snprintf(out->name, sizeof(out->name), "%s", name);
                state = ST_SCREEN;
            } else if (state == ST_SCREEN) {
                in_known_widget = 1;
                if (out->widget_count >= PURR_UI_MAX_WIDGETS) {
                    ESP_LOGW(TAG, "line %d: too many widgets, ignoring '%s'", line_no, name);
                    cur = NULL;
                } else if (strcmp(word, "label") == 0 || strcmp(word, "button") == 0 ||
                          strcmp(word, "image") == 0 || strcmp(word, "app_grid") == 0 ||
                          strcmp(word, "navbar") == 0 || strcmp(word, "panel") == 0 ||
                          strcmp(word, "carousel") == 0 || strcmp(word, "overview") == 0 ||
                          strcmp(word, "minimize") == 0 || strcmp(word, "gesture") == 0) {
                    cur = &out->widgets[out->widget_count++];
                    memset(cur, 0, sizeof(*cur));
                    cur->type = strcmp(word, "button") == 0 ? PURR_UI_WIDGET_BUTTON
                               : strcmp(word, "image") == 0 ? PURR_UI_WIDGET_IMAGE
                               : strcmp(word, "app_grid") == 0 ? PURR_UI_WIDGET_APP_GRID
                               : strcmp(word, "navbar") == 0 ? PURR_UI_WIDGET_NAVBAR
                               : strcmp(word, "panel") == 0 ? PURR_UI_WIDGET_PANEL
                               : strcmp(word, "carousel") == 0 ? PURR_UI_WIDGET_CAROUSEL
                               : strcmp(word, "overview") == 0 ? PURR_UI_WIDGET_OVERVIEW
                               : strcmp(word, "minimize") == 0 ? PURR_UI_WIDGET_MINIMIZE
                               : strcmp(word, "gesture") == 0 ? PURR_UI_WIDGET_GESTURE
                               : PURR_UI_WIDGET_LABEL;
                    cur->color = PURR_UI_COLOR_UNSET;
                    snprintf(cur->name, sizeof(cur->name), "%s", name);
                } else {
                    ESP_LOGW(TAG, "line %d: unknown widget type '%s', ignoring", line_no, word);
                    cur = NULL;
                }
                state = ST_WIDGET;
            } else {
                ESP_LOGW(TAG, "line %d: blocks don't nest that deep", line_no);
                return ESP_ERR_INVALID_ARG;
            }
            continue;
        }

        /* A property line. */
        char key[24];
        char *vals[4];
        int n = parse_property(t, key, sizeof(key), vals, 4);
        if (n < 0) {
            ESP_LOGW(TAG, "line %d: expected 'key = value', got '%s'", line_no, t);
            return ESP_ERR_INVALID_ARG;
        }
        if (state == ST_SCREEN && n >= 1 && strcmp(key, "background") == 0) {
            if (parse_color(vals[0], &out->background) != 0) {
                ESP_LOGW(TAG, "line %d: '%s' isn't a #RRGGBB color", line_no, vals[0]);
            }
        } else if (state == ST_WIDGET) {
            (void)in_known_widget;
            if (cur == NULL) {
                continue;                  /* inside an ignored block -- drop the property */
            }
            if (n >= 2 && strcmp(key, "position") == 0) {
                cur->x = atoi(vals[0]);
                cur->y = atoi(vals[1]);
            } else if (n >= 2 && strcmp(key, "size") == 0) {
                cur->w = atoi(vals[0]);
                cur->h = atoi(vals[1]);
            } else if (n >= 1 && strcmp(key, "size") == 0) {
                /* panel: one value, its thickness -- the rest of its rect is derived from
                 * the screen size and `edge` at build time, not stored here. */
                cur->h = atoi(vals[0]);
            } else if (n >= 1 && strcmp(key, "edge") == 0) {
                snprintf(cur->edge, sizeof(cur->edge), "%s", vals[0]);
            } else if (n >= 1 && (strcmp(key, "text") == 0 || strcmp(key, "label") == 0)) {
                snprintf(cur->text, sizeof(cur->text), "%s", vals[0]);
            } else if (n >= 1 && strcmp(key, "asset") == 0) {
                snprintf(cur->asset, sizeof(cur->asset), "%s", vals[0]);
            } else if (n >= 1 && strcmp(key, "color") == 0) {
                if (parse_color(vals[0], &cur->color) != 0) {
                    ESP_LOGW(TAG, "line %d: '%s' isn't a #RRGGBB color", line_no, vals[0]);
                }
            } else if (n >= 1 && strcmp(key, "scale") == 0) {
                cur->scale = atoi(vals[0]);
            } else if (n >= 1 && (strcmp(key, "columns") == 0 || strcmp(key, "capacity") == 0 ||
                                  strcmp(key, "close_lift") == 0 ||
                                  strcmp(key, "tolerance") == 0)) {
                cur->columns = atoi(vals[0]);
            } else if (n >= 1 && (strcmp(key, "hold_ms") == 0 || strcmp(key, "slide_ms") == 0)) {
                cur->anim_duration_ms = atoi(vals[0]);
            } else if (n >= 3 && strcmp(key, "step") == 0) {
                cur->step_x = atoi(vals[0]);
                cur->step_shrink_w = atoi(vals[1]);
                cur->step_shrink_h = atoi(vals[2]);
            } else if (n >= 1 && strcmp(key, "max_side_steps") == 0) {
                cur->max_side_steps = atoi(vals[0]);
            } else if (n >= 3 && strcmp(key, "zone") == 0) {
                /* minimize only: height from the bottom edge, then the x range the drag has
                 * to start inside of -- reuses w/x/y, none of which minimize needs for
                 * anything else. */
                cur->w = atoi(vals[0]);
                cur->x = atoi(vals[1]);
                cur->y = atoi(vals[2]);
            } else if (n >= 3 && strcmp(key, "animation") == 0) {
                snprintf(cur->anim_type, sizeof(cur->anim_type), "%s", vals[0]);
                cur->anim_duration_ms = atoi(vals[1]);
                snprintf(cur->anim_easing, sizeof(cur->anim_easing), "%s", vals[2]);
            } else {
                ESP_LOGW(TAG, "line %d: unknown or malformed property '%s', skipping",
                         line_no, key);
            }
        } else {
            ESP_LOGW(TAG, "line %d: property outside any block", line_no);
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (state != ST_TOP) {
        ESP_LOGW(TAG, "unclosed block at end of file");
        return ESP_ERR_INVALID_ARG;
    }
    if (out->name[0] == '\0') {
        ESP_LOGW(TAG, "no screen block found");
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/* ---------------------------------------------------------------- loading from flash */

typedef struct { char *buf; uint32_t cap; uint32_t got; } read_ctx_t;

static int read_cb(void *ctx, const void *data, uint32_t len)
{
    read_ctx_t *c = ctx;
    if (c->got + len >= c->cap) {          /* leave room for the NUL terminator */
        return -1;
    }
    memcpy(c->buf + c->got, data, len);
    c->got += len;
    return 0;
}

esp_err_t purr_ui_config_load(const char *path, purr_ui_screen_t *out)
{
    purr_fs_t *fs = purr_login_fs();
    int is_dir;
    uint32_t size;
    if (purr_fs_stat(fs, path, &is_dir, &size) != 0 || is_dir) {
        ESP_LOGW(TAG, "%s: not found", path);
        return ESP_ERR_NOT_FOUND;
    }

    char *buf = heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = malloc(size + 1);
    }
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    read_ctx_t rc = {buf, size + 1, 0};
    esp_err_t result;
    if (purr_fs_read(fs, path, read_cb, &rc) != 0) {
        ESP_LOGW(TAG, "%s: read failed", path);
        result = ESP_FAIL;
    } else {
        buf[rc.got] = '\0';
        result = purr_ui_config_parse(buf, out);
    }
    free(buf);
    return result;
}

/* ---------------------------------------------------------------- building the screen
 *
 * Chunk B (UI/SPEC.md section 2/4's "no app call reaches LVGL outside the service"): this
 * used to call lv_* directly. It now only ever talks to purr_ui_service_request() -- the
 * actual widget construction moved to purr_ui_service.c, the one file left that may call
 * lv_* at all. Proven here against this, the one real caller that existed before any app
 * did. */

/* Launcher3-style grid (chunk F): expands to one button per installed app, left to right
 * then top to bottom, `columns` per row. purr_apps_scan_current_user() is a fresh rescan
 * every time this screen builds -- no caching, same "the filesystem is the source of truth"
 * reasoning AppManager/SPEC.md section 4 already applies to the registry itself. Visual
 * only: the synthesized button's tap just logs for now (purr_ui_service.c's own
 * button_clicked_cb), same placeholder chunk B left for every button until an app exists to
 * own the event -- now an app exists, but launching from a tap with no console to print to
 * is real, separate scope (chunk F's own scoping note), not done here. */
static void build_app_grid(const purr_ui_widget_t *grid)
{
    purr_app_registry_t reg;
    purr_apps_scan_current_user(&reg);

    int columns = grid->columns > 0 ? grid->columns : 3;
    int cell_w = grid->w > 0 ? grid->w : 80;
    int cell_h = grid->h > 0 ? grid->h : 80;

    if (reg.count == 0) {
        ESP_LOGI(TAG, "app_grid '%s': no apps installed yet", grid->name);
    }

    for (int i = 0; i < reg.count; i++) {
        purr_ui_widget_t btn = {0};
        btn.type = PURR_UI_WIDGET_BUTTON;
        snprintf(btn.name, sizeof(btn.name), "%s", reg.apps[i].name);
        snprintf(btn.text, sizeof(btn.text), "%s", reg.apps[i].name);
        btn.color = PURR_UI_COLOR_UNSET;
        btn.x = grid->x + (i % columns) * cell_w;
        btn.y = grid->y + (i / columns) * cell_h;
        btn.w = cell_w;
        btn.h = cell_h;

        purr_ui_request_t req = {.op = PURR_UI_OP_ADD_WIDGET, .widget = btn};
        purr_ui_service_request(&req, NULL);
    }
}

/* KitKat-style nav bar (chunk G): exactly three fixed buttons, evenly spaced across the
 * bar's width -- Back/Home/Recents, hardcoded, not configurable per button (the "deliberately
 * small, not a general-purpose language" rule this whole file already follows for every
 * other widget). Visual only -- see purr_ui_config.h's own note on why even Home isn't
 * wired yet. */
static void build_navbar(const purr_ui_widget_t *bar)
{
    static const char *const labels[3] = {"Back", "Home", "Recents"};
    static const char *const names[3] = {"navbar_back", "navbar_home", "navbar_recents"};
    int cell_w = bar->w > 0 ? bar->w / 3 : 100;
    int cell_h = bar->h > 0 ? bar->h : 40;

    for (int i = 0; i < 3; i++) {
        purr_ui_widget_t btn = {0};
        btn.type = PURR_UI_WIDGET_BUTTON;
        snprintf(btn.name, sizeof(btn.name), "%s", names[i]);
        snprintf(btn.text, sizeof(btn.text), "%s", labels[i]);
        btn.color = PURR_UI_COLOR_UNSET;
        btn.x = bar->x + i * cell_w;
        btn.y = bar->y;
        btn.w = cell_w;
        btn.h = cell_h;

        purr_ui_request_t req = {.op = PURR_UI_OP_ADD_WIDGET, .widget = btn};
        purr_ui_service_request(&req, NULL);
    }
}

void purr_ui_build_screen(const purr_ui_screen_t *screen)
{
    purr_ui_request_t req = {.op = PURR_UI_OP_CLEAR_SCREEN};
    purr_ui_service_request(&req, NULL);

    if (screen->background != PURR_UI_COLOR_UNSET) {
        req = (purr_ui_request_t){.op = PURR_UI_OP_SET_BACKGROUND};
        req.widget.color = screen->background;
        purr_ui_service_request(&req, NULL);
    }

    for (int i = 0; i < screen->widget_count; i++) {
        if (screen->widgets[i].type == PURR_UI_WIDGET_APP_GRID) {
            build_app_grid(&screen->widgets[i]);
            continue;
        }
        if (screen->widgets[i].type == PURR_UI_WIDGET_NAVBAR) {
            build_navbar(&screen->widgets[i]);
            continue;
        }
        req = (purr_ui_request_t){.op = PURR_UI_OP_ADD_WIDGET, .widget = screen->widgets[i]};
        purr_ui_service_request(&req, NULL);
    }
}

esp_err_t purr_ui_load_and_build(const char *path)
{
    purr_ui_screen_t screen;
    esp_err_t e = purr_ui_config_load(path, &screen);
    if (e != ESP_OK) {
        purr_lvgl_report_critical_error("ui config missing or invalid");
        return e;
    }
    purr_ui_build_screen(&screen);
    return ESP_OK;
}

/* ---------------------------------------------------------------- default config */

static const char s_default_shell_conf[] =
    "# Default PURR OS shell screen. Edit freely -- netinstall never overwrites a file\n"
    "# that's already here (purr_ui_config_setup()'s own missing-only check), and neither\n"
    "# does this planting step on a later boot.\n"
    "screen \"shell\" {\n"
    "    background = #000000\n"
    "\n"
    "    label \"title\" {\n"
    "        text      = \"PURR OS\"\n"
    "        position  = 16, 16\n"
    "        color     = #fda000\n"
    "        scale     = 3\n"
    "        animation = fade_in, 400, ease_out\n"
    "    }\n"
    "\n"
    "    label \"subtitle\" {\n"
    "        text      = \"LVGL shell -- config-driven UI\"\n"
    "        position  = 16, 60\n"
    "        color     = #94b2b2\n"
    "        animation = fade_in, 600, ease_out\n"
    "    }\n"
    "\n"
    "    app_grid \"apps\" {\n"
    "        position = 16, 100\n"
    "        size     = 80, 70\n"
    "        columns  = 3\n"
    "    }\n"
    "\n"
    "    navbar \"nav\" {\n"
    "        position = 0, 200\n"
    "        size     = 320, 40\n"
    "    }\n"
    "}\n";

void purr_ui_config_setup(purr_cli_t *cli)
{
    purr_fs_t *fs = purr_login_fs();
    static const char path[] = "/ui/shell.conf";
    if (purr_fs_stat(fs, path, NULL, NULL) == 0) {
        return;                            /* already there -- never overwrite */
    }
    purr_fs_mkdir(fs, "/ui");              /* ignore the error if it already exists */
    if (purr_fs_write(fs, path, s_default_shell_conf, sizeof(s_default_shell_conf) - 1) != 0) {
        purr_cli_puts(cli, "ui: could not plant the default shell.conf\n");
        return;
    }
    purr_cli_puts(cli, "ui: planted the default /ui/shell.conf\n");
}

/* Default /ui/cardshell.conf -- exactly the values that used to be hardcoded #defines and
 * literal colors in purr_ui_service.c's build_card_shell() (SHELL_TOP_H=70/SHELL_BOTTOM_H=60/
 * SHELL_LEFT_W=70 and the three panel colors), so planting this changes nothing visible on a
 * fresh device; it's the structural piece of the card shell (which edges have panels, their
 * thickness/color, the title) that's data now instead of compiled constants. The gesture
 * engine itself (drag/reveal/carousel/overview/minimize) is still compiled C -- see
 * memory/[[project_card_shell_ui_redesign]]'s own note on why only this part moved first. */
static const char s_default_cardshell_conf[] =
    "# Card shell structural config. Edit freely -- netinstall never overwrites a file\n"
    "# that's already here, and neither does this planting step on a later boot.\n"
    "#\n"
    "# The gesture engine (drag/reveal/carousel/multitasking overview/minimize animation)\n"
    "# is compiled code, not described here -- this file covers which edges have panels,\n"
    "# how thick/what color they are, the title, and a few tuning numbers for the carousel/\n"
    "# overview/minimize animation.\n"
    "screen \"cardshell\" {\n"
    "    panel \"notifications\" {\n"
    "        edge  = top\n"
    "        size  = 70\n"
    "        color = #2a2a3a\n"
    "        text  = \"Notifications\"\n"
    "    }\n"
    "\n"
    "    panel \"apps\" {\n"
    "        edge  = bottom\n"
    "        size  = 60\n"
    "        color = #8a3fd1\n"
    "    }\n"
    "\n"
    "    panel \"status\" {\n"
    "        edge  = left\n"
    "        size  = 70\n"
    "        color = #1e5a4a\n"
    "    }\n"
    "\n"
    "    label \"title\" {\n"
    "        text     = \"[AdultSwim]\"\n"
    "        position = 16, 12\n"
    "        color    = #fda000\n"
    "        scale    = 2\n"
    "    }\n"
    "\n"
    "    carousel \"apps_open\" {\n"
    "        capacity = 12\n"
    "    }\n"
    "\n"
    "    overview \"multitask\" {\n"
    "        hold_ms        = 450\n"
    "        size           = 200, 160\n"
    "        step           = 60, 30, 24\n"
    "        max_side_steps = 2\n"
    "        close_lift     = 20\n"
    "    }\n"
    "\n"
    "    minimize \"home_slide\" {\n"
    "        slide_ms = 300\n"
    "        # zone: height from the bottom edge, then the x range (min, max) a swipe-up has\n"
    "        # to start inside of to be recognized as \"go home\" at all\n"
    "        zone     = 48, 80, 240\n"
    "    }\n"
    "\n"
    "    gesture \"touch\" {\n"
    "        tolerance = 3\n"
    "    }\n"
    "}\n";

void purr_ui_card_shell_config_setup(purr_cli_t *cli)
{
    purr_fs_t *fs = purr_login_fs();
    static const char path[] = "/ui/cardshell.conf";
    if (purr_fs_stat(fs, path, NULL, NULL) == 0) {
        return;                            /* already there -- never overwrite */
    }
    purr_fs_mkdir(fs, "/ui");              /* ignore the error if it already exists */
    if (purr_fs_write(fs, path, s_default_cardshell_conf,
                      sizeof(s_default_cardshell_conf) - 1) != 0) {
        purr_cli_puts(cli, "ui: could not plant the default cardshell.conf\n");
        return;
    }
    purr_cli_puts(cli, "ui: planted the default /ui/cardshell.conf\n");
}

/* ---------------------------------------------------------------- UI mode selector
 *
 * Which top-level engine main.c boots into -- a fixed, compiled choice between two real
 * screens (card_shell / nav), not an open-ended format. purr_ui_config.h's own note has the
 * full reasoning (the Hyprland precedent: compiled layouts, picked and tuned by config). */
#define PURR_UI_MODE_PATH "/ui/mode.conf"

purr_ui_mode_t purr_ui_mode_get(void)
{
    purr_fs_t *fs = purr_login_fs();
    char buf[16];
    read_ctx_t rc = {buf, sizeof(buf), 0};
    if (purr_fs_read(fs, PURR_UI_MODE_PATH, read_cb, &rc) != 0) {
        return PURR_UI_MODE_CARD_SHELL;        /* missing or unreadable -- the safe default */
    }
    buf[rc.got] = '\0';
    /* trim() only strips spaces/tabs -- its one existing caller already splits on '\n'
     * before ever calling it, so it never needed to. This is a whole-file read, not a
     * pre-split line, so the trailing newline has to go first or it never matches "nav". */
    char *nl = strchr(buf, '\n');
    if (nl != NULL) {
        *nl = '\0';
    }
    char *t = trim(buf);
    if (strcmp(t, "nav") == 0) {
        return PURR_UI_MODE_NAV;
    }
    if (strcmp(t, "w10m") == 0) {
        return PURR_UI_MODE_W10M;
    }
    return PURR_UI_MODE_CARD_SHELL;            /* "card_shell", empty, or anything unrecognized */
}

void purr_ui_mode_setup(purr_cli_t *cli)
{
    purr_fs_t *fs = purr_login_fs();
    static const char path[] = PURR_UI_MODE_PATH;
    if (purr_fs_stat(fs, path, NULL, NULL) == 0) {
        return;                            /* already there -- never overwrite */
    }
    purr_fs_mkdir(fs, "/ui");              /* ignore the error if it already exists */
    static const char default_mode[] = CONFIG_PURR_UI_DEFAULT_MODE "\n";
    if (purr_fs_write(fs, path, default_mode, sizeof(default_mode) - 1) != 0) {
        purr_cli_puts(cli, "ui: could not plant the default mode.conf\n");
        return;
    }
    purr_cli_puts(cli, "ui: planted the default /ui/mode.conf\n");
}
