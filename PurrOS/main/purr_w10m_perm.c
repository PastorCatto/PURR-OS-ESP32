/*
 * purr_w10m_perm.c - the permission prompt (Permissions/SPEC.md section 6, plan10.md).
 *
 * "<app> wants to <do something>. Allow always / Just once / Don't allow." One card over a dimmed screen; the
 * app that asked is told the answer as PURR_APPEV_PERM. After two refusals the third button offers "Don't ask
 * again". Prompts for other requests wait in the runtime's queue and appear one at a time.
 */
#include <stdio.h>
#include <string.h>

#include "lvgl.h"

#include "purr_catrt_power.h"
#include "purr_perm.h"
#include "purr_ui_service.h"
#include "purr_w10m_priv.h"
#include "purr_w10m_theme.h"

static lv_obj_t *s_scrim, *s_card, *s_text, *s_deny_label;
static lv_obj_t *s_btn_parent;                  /* where the three buttons go: the card (stacked) or a row of their own (the Android skin) */
static int s_never;                             /* the third button currently means "don't ask again" */
static lv_obj_t *s_btns[3];                     /* the three buttons, for a board with no touch screen (w10m_perm_key) */
static int s_nbtn, s_focus = 2;                 /* which one the d-pad has highlighted: it starts on "Don't allow" */

static void perm_focus(int i)
{
    for (int k = 0; k < s_nbtn; k++) {
        lv_obj_remove_state(s_btns[k], LV_STATE_FOCUSED);
    }
    s_focus = i;
    if (i >= 0 && i < s_nbtn) {
        lv_obj_add_state(s_btns[i], LV_STATE_FOCUSED);
    }
}

static void fill(const char *app, int perm, int denials)
{
    const char *what = purr_perm_describe(perm);
    lv_label_set_text_fmt(s_text, "%.24s wants to %s.", app, what ? what : "do something");
    s_never = denials >= PURR_PERM_DENIALS_BEFORE_NEVER;
    lv_label_set_text(s_deny_label, s_never ? "Don't ask again" : "Don't allow");
    lv_obj_remove_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_scrim);
    perm_focus(2);                                      /* "Don't allow": a stray Enter never grants anything */
}

static void answered(lv_event_t *e)
{
    int choice = (int)(intptr_t)lv_event_get_user_data(e);
    if (choice == PURR_ANSWER_DENY && s_never) {
        choice = PURR_ANSWER_NEVER;
    }
    char app[32];
    int perm = 0, denials = 0;
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    if (purr_catrt_perm_answer(choice, app, sizeof(app), &perm, &denials)) {
        fill(app, perm, denials);                       /* we are on the UI task: show the next one directly */
    }
}

static lv_obj_t *button(const char *text, int choice, lv_obj_t **label_out)
{
    int android = purr_theme_skin() == PURR_SKIN_ANDROID;
    lv_obj_t *b = lv_button_create(s_btn_parent);
    if (android) {                                      /* three across, side by side */
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_height(b, 26);
        lv_obj_set_style_pad_all(b, 0, 0);
    } else {
        lv_obj_set_width(b, LV_PCT(100));
    }
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, android ? &lv_font_montserrat_12 : &lv_font_montserrat_14, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, answered, LV_EVENT_CLICKED, (void *)(intptr_t)choice);
    lv_obj_set_style_outline_width(b, 2, LV_STATE_FOCUSED);      /* the d-pad's highlight */
    lv_obj_set_style_outline_color(b, lv_color_white(), LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(b, 1, LV_STATE_FOCUSED);
    if (s_nbtn < 3) {
        s_btns[s_nbtn++] = b;
    }
    if (label_out) {
        *label_out = l;
    }
    return b;
}

static void build(void)
{
    s_scrim = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_scrim);
    lv_obj_set_size(s_scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scrim, LV_OPA_60, 0);
    lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_CLICKABLE);    /* swallows taps: the question has to be answered */
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);

    int android = purr_theme_skin() == PURR_SKIN_ANDROID;
    s_card = lv_obj_create(s_scrim);
    lv_obj_remove_style_all(s_card);
    purr_theme_apply(s_card, PURR_TH_SURFACE);
    lv_obj_set_width(s_card, LV_PCT(88));
    lv_obj_set_height(s_card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_card, android ? 0 : 10, 0);
    lv_obj_set_style_pad_row(s_card, android ? 0 : 6, 0);
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(s_card);
    s_btn_parent = s_card;

    lv_obj_t *title;
    if (android) {
        /* Froyo's dialog: a dark rounded panel, the app's small icon and the title over an orange line, the question, and the three buttons in a row on a grey bar. */
        lv_obj_set_style_radius(s_card, 8, 0);
        lv_obj_set_style_clip_corner(s_card, true, 0);
        lv_obj_t *head = lv_obj_create(s_card);
        lv_obj_remove_style_all(head);
        purr_theme_apply(head, PURR_TH_TITLE_BAR);
        lv_obj_set_size(head, LV_PCT(100), 28);
        lv_obj_set_style_border_width(head, 2, 0);
        lv_obj_set_style_border_color(head, purr_theme_accent_color(), 0);
        lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);
        const lv_image_dsc_t *ic = w10m_img("fr_s_app");
        if (ic != NULL) {
            lv_obj_t *im = lv_image_create(head);
            lv_image_set_src(im, ic);
            lv_obj_align(im, LV_ALIGN_LEFT_MID, 8, 0);
        }
        title = lv_label_create(head);
        lv_obj_align(title, LV_ALIGN_LEFT_MID, ic != NULL ? 30 : 8, 0);
        lv_obj_set_style_text_color(title, lv_color_white(), 0);
    } else {
        title = lv_label_create(s_card);
        purr_theme_apply(title, PURR_TH_ACCENT_TEXT);
    }
    lv_label_set_text(title, "Permission");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    s_text = lv_label_create(s_card);
    lv_label_set_long_mode(s_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_text, LV_PCT(100));
    lv_obj_set_style_text_font(s_text, &lv_font_montserrat_14, 0);
    if (android) {
        lv_obj_set_style_pad_all(s_text, 10, 0);
    }
    purr_theme_apply(s_text, PURR_TH_TEXT);

    if (android) {                                      /* the buttons sit in a bar of their own */
        s_btn_parent = lv_obj_create(s_card);
        lv_obj_remove_style_all(s_btn_parent);
        lv_obj_set_size(s_btn_parent, LV_PCT(100), 38);
        lv_obj_set_style_bg_color(s_btn_parent, lv_color_hex(0x4A4A4A), 0);
        lv_obj_set_style_bg_grad_color(s_btn_parent, lv_color_hex(0x2A2A2A), 0);
        lv_obj_set_style_bg_grad_dir(s_btn_parent, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(s_btn_parent, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(s_btn_parent, 6, 0);
        lv_obj_set_style_pad_column(s_btn_parent, 4, 0);
        lv_obj_set_flex_flow(s_btn_parent, LV_FLEX_FLOW_ROW);
        lv_obj_clear_flag(s_btn_parent, LV_OBJ_FLAG_SCROLLABLE);
    }

    button("Allow always", PURR_ANSWER_ALWAYS, NULL);
    button("Just once", PURR_ANSWER_ONCE, NULL);
    button("Don't allow", PURR_ANSWER_DENY, &s_deny_label);
}

typedef struct {
    const char *app;
    int perm, denials;
} ask_t;

static void *show_cb(void *arg)
{
    ask_t *a = arg;
    if (s_scrim == NULL) {
        build();
    }
    fill(a->app, a->perm, a->denials);
    return NULL;
}

/* Any task: the runtime calls this on the asking app's task; the card is built and shown on the UI task. */
static void show(const char *app, int perm, int denials)
{
    ask_t a = {app, perm, denials};
    purr_ui_service_run(show_cb, &a, NULL);
}

int w10m_perm_is_open(void)
{
    return s_scrim != NULL && !lv_obj_has_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
}

/* The prompt for a board with no touch screen (a gamepad): up and left, or down and right, move the highlight over the three buttons, Enter answers with the
 * highlighted one, Esc and Back say "Don't allow". Without this the prompt could not be answered at all, and it is modal, so nothing else could be done either.
 * While it is open every key is its own: 1 always (0 only when it is not open). A shell calls this first from its key hook. */
int w10m_perm_key(uint32_t key)
{
    if (!w10m_perm_is_open() || s_nbtn < 3) {
        return 0;
    }
    switch (key) {
    case LV_KEY_UP:
    case LV_KEY_LEFT:
    case LV_KEY_PREV:
        perm_focus((s_focus + 2) % 3);
        break;
    case LV_KEY_DOWN:
    case LV_KEY_RIGHT:
    case LV_KEY_NEXT:
        perm_focus((s_focus + 1) % 3);
        break;
    case LV_KEY_ENTER:
        lv_obj_send_event(s_btns[s_focus], LV_EVENT_CLICKED, NULL);
        break;
    case LV_KEY_ESC:
        lv_obj_send_event(s_btns[2], LV_EVENT_CLICKED, NULL);
        break;
    default:
        break;
    }
    return 1;
}

void w10m_perm_init(void)
{
    purr_catrt_perm_set_ui(show);
}
