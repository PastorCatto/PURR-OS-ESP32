// calculator.c — PURR OS built-in calculator app (.paws)
// Uses purr_win.h — works on both KittenUI (LVGL) and MiniWin.

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "purr_win.h"
#include "purr_module.h"

// ── State ─────────────────────────────────────────────────────────────────────

static purr_win_t  s_win     = 0;
static purr_wid_t  s_display = 0;   // top label showing current value / expression

static double  s_accum    = 0.0;  // accumulated value
static double  s_operand  = 0.0;  // current entry
static char    s_op       = 0;    // pending operator: + - * /
static bool    s_fresh    = true; // true = next digit starts a new number
static bool    s_decimal  = false;
static int     s_dec_pos  = 1;    // divisor for decimal entry
static char    s_entry[24];       // display buffer for current entry

// ── Display update ────────────────────────────────────────────────────────────

static void refresh(void) {
    purr_win_label_set(s_display, s_entry);
}

static void entry_set(double v) {
    if (v == (long long)v && fabs(v) < 1e12)
        snprintf(s_entry, sizeof(s_entry), "%.0f", v);
    else
        snprintf(s_entry, sizeof(s_entry), "%.6g", v);
    refresh();
}

static void entry_reset(void) {
    s_operand = 0.0;
    s_decimal = false;
    s_dec_pos = 1;
    s_fresh   = true;
    strncpy(s_entry, "0", sizeof(s_entry));
    refresh();
}

// ── Button callbacks ──────────────────────────────────────────────────────────

static void on_digit(purr_wid_t wid, purr_event_t event, void *user) {
    (void)wid; (void)event;
    int digit = (int)(intptr_t)user;

    if (s_fresh) {
        s_operand = 0.0;
        s_decimal = false;
        s_dec_pos = 1;
        strncpy(s_entry, "0", sizeof(s_entry));
        s_fresh = false;
    }

    if (!s_decimal) {
        s_operand = s_operand * 10.0 + digit;
    } else {
        s_dec_pos *= 10;
        s_operand += (double)digit / (double)s_dec_pos;
    }

    entry_set(s_operand);
}

static void on_dot(purr_wid_t wid, purr_event_t event, void *user) {
    (void)wid; (void)event; (void)user;
    if (s_fresh) { s_operand = 0.0; s_fresh = false; }
    if (!s_decimal) {
        s_decimal = true;
        s_dec_pos = 1;
        // append "." to display if not already there
        size_t l = strlen(s_entry);
        if (l < sizeof(s_entry) - 2) { s_entry[l] = '.'; s_entry[l+1] = '\0'; }
        refresh();
    }
}

static void apply_op(void) {
    switch (s_op) {
    case '+': s_accum += s_operand; break;
    case '-': s_accum -= s_operand; break;
    case '*': s_accum *= s_operand; break;
    case '/':
        if (s_operand != 0.0) s_accum /= s_operand;
        else { strncpy(s_entry, "ERR:DIV0", sizeof(s_entry)); refresh(); return; }
        break;
    default: s_accum = s_operand; break;
    }
    entry_set(s_accum);
}

static void on_op(purr_wid_t wid, purr_event_t event, void *user) {
    (void)wid; (void)event;
    char op = *(const char *)user;
    // If no digit was entered since the last operator/equals (e.g. pressing
    // "+" then "-" before typing a number), just swap the pending operator
    // instead of re-applying the previous one against the same operand —
    // otherwise "5, +, -, 3, =" silently double-counted the first operand.
    if (!s_fresh) apply_op();
    s_op    = op;
    s_fresh = true;
}

static void on_equals(purr_wid_t wid, purr_event_t event, void *user) {
    (void)wid; (void)event; (void)user;
    apply_op();
    s_op    = 0;
    s_fresh = true;
}

static void on_clear(purr_wid_t wid, purr_event_t event, void *user) {
    (void)wid; (void)event; (void)user;
    s_accum = 0.0;
    s_op    = 0;
    entry_reset();
}

static void on_sign(purr_wid_t wid, purr_event_t event, void *user) {
    (void)wid; (void)event; (void)user;
    s_operand = -s_operand;
    entry_set(s_operand);
}

static void on_pct(purr_wid_t wid, purr_event_t event, void *user) {
    (void)wid; (void)event; (void)user;
    s_operand /= 100.0;
    entry_set(s_operand);
}

// ── Operator chars (static storage for callback user pointers) ────────────────

static const char OPS[] = "+-*/";

// ── Build UI ──────────────────────────────────────────────────────────────────

static int calculator_init(void) {
    s_accum   = 0.0;
    s_operand = 0.0;
    s_op      = 0;
    s_fresh   = true;
    s_decimal = false;
    s_dec_pos = 1;
    strncpy(s_entry, "0", sizeof(s_entry));

    s_win     = purr_win_create("Calculator");
    s_display = purr_win_label(s_win, "0");
    purr_win_label_align(s_display, PURR_ALIGN_RIGHT);

    // Row 0: C  +/-  %  /
    purr_wid_t r0 = purr_win_row(s_win, 4);
    purr_win_button(s_win, "C",   on_clear, NULL);
    purr_win_button(s_win, "+/-", on_sign,  NULL);
    purr_win_button(s_win, "%",   on_pct,   NULL);
    purr_win_button(s_win, "/",   on_op,    (void *)&OPS[3]);
    purr_win_layout_end(r0);

    // Row 1: 7 8 9 *
    purr_wid_t r1 = purr_win_row(s_win, 4);
    purr_win_button(s_win, "7", on_digit, (void *)(intptr_t)7);
    purr_win_button(s_win, "8", on_digit, (void *)(intptr_t)8);
    purr_win_button(s_win, "9", on_digit, (void *)(intptr_t)9);
    purr_win_button(s_win, "*", on_op,    (void *)&OPS[2]);
    purr_win_layout_end(r1);

    // Row 2: 4 5 6 -
    purr_wid_t r2 = purr_win_row(s_win, 4);
    purr_win_button(s_win, "4", on_digit, (void *)(intptr_t)4);
    purr_win_button(s_win, "5", on_digit, (void *)(intptr_t)5);
    purr_win_button(s_win, "6", on_digit, (void *)(intptr_t)6);
    purr_win_button(s_win, "-", on_op,    (void *)&OPS[1]);
    purr_win_layout_end(r2);

    // Row 3: 1 2 3 +
    purr_wid_t r3 = purr_win_row(s_win, 4);
    purr_win_button(s_win, "1", on_digit, (void *)(intptr_t)1);
    purr_win_button(s_win, "2", on_digit, (void *)(intptr_t)2);
    purr_win_button(s_win, "3", on_digit, (void *)(intptr_t)3);
    purr_win_button(s_win, "+", on_op,    (void *)&OPS[0]);
    purr_win_layout_end(r3);

    // Row 4: 0  .  =
    purr_wid_t r4 = purr_win_row(s_win, 4);
    purr_win_button(s_win, "0", on_digit,  (void *)(intptr_t)0);
    purr_win_button(s_win, ".", on_dot,    NULL);
    purr_win_button(s_win, "=", on_equals, NULL);
    purr_win_layout_end(r4);

    purr_win_show(s_win);
    return 0;
}

static void calculator_deinit(void) {
    purr_win_destroy(s_win);
    s_win = 0; s_display = 0;
}

// ── Module header ─────────────────────────────────────────────────────────────

PURR_MODULE_REGISTER(calculator) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_APP,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "calculator",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = calculator_init,
    .deinit            = calculator_deinit,
};
