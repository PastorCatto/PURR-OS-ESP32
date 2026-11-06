/*
 * calculator.c - Calculator, a sideloaded UI app: the four operations, percent and sign change. No libc: only the catcalls it is handed.
 *
 * A grid of 4 by 5 buttons under a small line that shows the pending sum and the result in the 28 point face. Numbers are whole numbers of thousandths (three decimals), kept in
 * 64 bits, so there is no floating point. Operations work left to right, like a basic desk calculator: 2 + 3 x 4 = 20. Operands up to about 3,000,000 can be multiplied or divided,
 * sums can go higher; anything outside says "Too big". Dividing by zero says "Cannot divide by 0". C clears everything, Del removes the last typed character.
 *
 * Needs the `ui` catcall at version 3 (the grid), OS 1.0.23 or newer; on an older OS it says so.
 */
#include <stddef.h>
#include <stdint.h>

#include "purr_app_abi.h"
#include "purr_catcalls.h"

#define NO_OPT __attribute__((noinline, optimize("no-tree-loop-distribute-patterns")))

typedef long long i64;
typedef unsigned long long u64;

#define SCALE 1000LL                                /* three decimals */
#define MAX_OPERAND 3000000000LL                    /* scaled: 3,000,000.000; a product of two of these still fits 64 bits */
#define MAX_SHOWN 9000000000000000LL                /* scaled: the largest value that is shown or added */
#define MAX_ENTRY 12                                /* characters typed, counting the point and the sign */

static const purr_cc_ui_t *ui;
static const purr_cc_app_t *app;

static const char *const s_key[20] = {
    "C", "+/-", "Del", "\xc3\xb7",                  /* the division sign */
    "7", "8", "9", "\xc3\x97",                      /* the multiplication sign */
    "4", "5", "6", "-",
    "1", "2", "3", "+",
    "0", ".", "=", "%",
};
static int s_h[20];                                 /* button handles */
static int h_expr, h_res;

static char s_entry[MAX_ENTRY + 2];                 /* what is being typed; empty = nothing typed yet */
static i64 s_acc;                                   /* the value so far */
static int s_op;                                    /* the pending operation: '+', '-', '*', '/' or 0 */
static int s_fresh;                                 /* the last key finished a number: the next digit starts a new one */
static int s_err;                                   /* 0, or 1 = too big, 2 = divide by zero */

/* ---------------------------------------------------------------- tiny helpers (no libc) */

NO_OPT static int slen(const char *s)
{
    int n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

NO_OPT static void put(char *dst, const char *src, int cap)
{
    int i = 0;
    while (src[i] && i < cap - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

NO_OPT static i64 iabs64(i64 v) { return v < 0 ? -v : v; }

/* A value as text: sign, whole part, then up to three decimals with the trailing zeros dropped. */
NO_OPT static void fmt_val(i64 v, char *out, int cap)
{
    int neg = v < 0;
    u64 m = (u64)iabs64(v);
    u64 ip = m / (u64)SCALE;
    int fr = (int)(m % (u64)SCALE);
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + (int)(ip % 10));
        ip /= 10;
    } while (ip != 0 && n < 20);
    int k = 0;
    if (neg && k < cap - 1) {
        out[k++] = '-';
    }
    while (n > 0 && k < cap - 1) {
        out[k++] = tmp[--n];
    }
    if (fr != 0 && k < cap - 5) {
        out[k++] = '.';
        int d = SCALE / 10;
        while (fr != 0) {
            out[k++] = (char)('0' + fr / d);
            fr %= d;
            d /= 10;
        }
    }
    out[k] = '\0';
}

/* The typed text as thousandths. */
NO_OPT static i64 parse_entry(const char *s)
{
    int neg = 0, i = 0, frac_digits = 0, in_frac = 0;
    i64 ip = 0, fr = 0;
    if (s[0] == '-') {
        neg = 1;
        i = 1;
    }
    for (; s[i]; i++) {
        if (s[i] == '.') {
            in_frac = 1;
        } else if (s[i] >= '0' && s[i] <= '9') {
            if (!in_frac) {
                ip = ip * 10 + (s[i] - '0');
            } else if (frac_digits < 3) {
                fr = fr * 10 + (s[i] - '0');
                frac_digits++;
            }
        }
    }
    while (frac_digits < 3) {
        fr *= 10;
        frac_digits++;
    }
    i64 v = ip * SCALE + fr;
    return neg ? -v : v;
}

NO_OPT static i64 cur(void)
{
    return s_entry[0] != '\0' ? parse_entry(s_entry) : s_acc;
}

/* a op b. Returns 0 and sets *r, or the error code (1 too big, 2 divide by zero). */
NO_OPT static int apply(int op, i64 a, i64 b, i64 *r)
{
    if (op == '+' || op == '-') {
        if (iabs64(a) > MAX_SHOWN || iabs64(b) > MAX_SHOWN) {
            return 1;
        }
        i64 v = op == '+' ? a + b : a - b;
        if (iabs64(v) > MAX_SHOWN) {
            return 1;
        }
        *r = v;
        return 0;
    }
    if (iabs64(a) > MAX_OPERAND || iabs64(b) > MAX_OPERAND) {
        return 1;
    }
    if (op == '*') {
        *r = (a * b) / SCALE;
        return 0;
    }
    if (b == 0) {
        return 2;
    }
    *r = (a * SCALE) / b;
    return 0;
}

/* ---------------------------------------------------------------- the display */

NO_OPT static void refresh(void)
{
    char res[24], expr[40];
    if (s_err == 1) {
        put(res, "Too big", sizeof(res));
    } else if (s_err == 2) {
        put(res, "Cannot divide by 0", sizeof(res));
    } else if (s_entry[0] != '\0') {
        put(res, s_entry, sizeof(res));
    } else {
        fmt_val(s_acc, res, sizeof(res));
    }
    expr[0] = '\0';
    if (s_op != 0 && s_err == 0) {
        fmt_val(s_acc, expr, sizeof(expr) - 4);
        int n = slen(expr);
        expr[n] = ' ';
        const char *sym = s_op == '*' ? "\xc3\x97" : s_op == '/' ? "\xc3\xb7" : s_op == '+' ? "+" : "-";
        put(expr + n + 1, sym, 4);
    }
    ui->set_text(h_expr, expr);
    ui->set_text(h_res, res);
}

NO_OPT static void clear_all(void)
{
    s_entry[0] = '\0';
    s_acc = 0;
    s_op = 0;
    s_fresh = 0;
    s_err = 0;
}

/* ---------------------------------------------------------------- the keys */

NO_OPT static void type_char(char c)
{
    if (s_err != 0) {
        clear_all();
    }
    if (s_fresh) {                                  /* a finished number is not extended: a new one starts */
        s_entry[0] = '\0';
        s_fresh = 0;
    }
    int n = slen(s_entry);
    if (c == '.') {
        for (int i = 0; i < n; i++) {
            if (s_entry[i] == '.') {
                return;                             /* only one point */
            }
        }
        if (n == 0) {
            s_entry[0] = '0';
            n = 1;
        }
    } else if (n == 1 && s_entry[0] == '0') {
        n = 0;                                      /* no leading zeros */
    }
    if (n >= MAX_ENTRY) {
        return;
    }
    s_entry[n] = c;
    s_entry[n + 1] = '\0';
}

NO_OPT static void do_operator(int op)
{
    if (s_err != 0) {
        return;
    }
    if (s_op != 0 && s_entry[0] != '\0') {          /* 2 + 3 x ...: finish 2 + 3 first, left to right */
        i64 r = 0;
        s_err = apply(s_op, s_acc, parse_entry(s_entry), &r);
        s_acc = r;
    } else if (s_entry[0] != '\0') {
        s_acc = parse_entry(s_entry);
    }
    s_entry[0] = '\0';
    s_op = op;
    s_fresh = 0;
}

NO_OPT static void do_equals(void)
{
    if (s_err != 0 || s_op == 0) {
        if (s_entry[0] != '\0') {
            s_acc = parse_entry(s_entry);
            s_entry[0] = '\0';
            s_fresh = 1;
        }
        return;
    }
    i64 r = 0;
    s_err = apply(s_op, s_acc, cur(), &r);
    s_acc = r;
    s_entry[0] = '\0';
    s_op = 0;
    s_fresh = 1;
}

NO_OPT static void press(int k)
{
    const char *t = s_key[k];
    if (t[0] >= '0' && t[0] <= '9' && t[1] == '\0') {
        type_char(t[0]);
    } else if (t[0] == '.') {
        type_char('.');
    } else if (t[0] == 'C') {
        clear_all();
    } else if (t[0] == 'D') {                       /* Del */
        if (s_err != 0) {
            clear_all();
        } else if (s_entry[0] != '\0') {
            s_entry[slen(s_entry) - 1] = '\0';
        }
    } else if (t[0] == '+' && t[1] == '/') {        /* +/- */
        if (s_err == 0) {
            if (s_entry[0] == '\0') {
                s_acc = -s_acc;
                s_fresh = 1;
            } else if (s_entry[0] == '-') {
                for (int i = 0; s_entry[i]; i++) {
                    s_entry[i] = s_entry[i + 1];
                }
            } else if (slen(s_entry) < MAX_ENTRY) {
                for (int i = slen(s_entry); i >= 0; i--) {
                    s_entry[i + 1] = s_entry[i];
                }
                s_entry[0] = '-';
            }
        }
    } else if (t[0] == '%') {                       /* a hundredth of what is on show */
        if (s_err == 0) {
            i64 v = cur() / 100;
            fmt_val(v, s_entry, sizeof(s_entry));
            if (s_entry[0] == '0' && s_entry[1] == '\0') {
                s_entry[0] = '\0';
                s_acc = 0;
            }
            s_fresh = 0;
        }
    } else if (t[0] == '=') {
        do_equals();
    } else if (t[0] == '+') {
        do_operator('+');
    } else if (t[0] == '-') {
        do_operator('-');
    } else if ((unsigned char)t[0] == 0xc3 && (unsigned char)t[1] == 0x97) {
        do_operator('*');
    } else if ((unsigned char)t[0] == 0xc3 && (unsigned char)t[1] == 0xb7) {
        do_operator('/');
    }
    refresh();
}

/* ---------------------------------------------------------------- the app */

static int on_create(const purr_app_env_t *env, const void *state, uint32_t len)
{
    (void)state;
    (void)len;
    if (env == NULL || env->catcall_get == NULL) {
        return -1;
    }
    app = (const purr_cc_app_t *)env->catcall_get("app", 1);
    ui = (const purr_cc_ui_t *)env->catcall_get("ui", 3);
    if (ui == NULL) {
        ui = (const purr_cc_ui_t *)env->catcall_get("ui", 1);
        if (ui != NULL) {
            ui->win_title("Calculator");
            ui->label("Calculator needs OS 1.0.23 or newer.", PURR_UI_ROLE_TITLE);
        }
        return ui != NULL ? 0 : -1;
    }
    clear_all();
    ui->win_title("Calculator");
    h_expr = ui->label("", PURR_UI_ROLE_DIM);
    h_res = ui->label("0", PURR_UI_ROLE_MEDIUM);
    int g = ui->grid(4, 22);
    for (int i = 0; i < 20; i++) {
        s_h[i] = ui->grid_button(g, s_key[i]);
    }
    refresh();
    return 0;
}

static void on_event(const purr_app_event_t *ev)
{
    if (ui == NULL || ui->grid == NULL || s_h[0] == 0) {
        return;                                     /* the "needs a newer OS" page has nothing to do */
    }
    if (ev->type != PURR_APPEV_UI || ev->name == NULL) {
        return;
    }
    const char *n = ev->name;
    if (n[0] == 'c' && n[1] == 'l' && n[2] == 'i' && n[3] == 'c' && n[4] == 'k' && n[5] == '\0') {
        for (int i = 0; i < 20; i++) {
            if (s_h[i] == ev->arg) {
                press(i);
                return;
            }
        }
    }
}

static void on_destroy(int reason)
{
    (void)reason;
}

static const purr_app_table2_t s_table = {
    .base = {.abi_version = PURR_APP_ABI_VERSION_2, .on_create = on_create, .on_destroy = on_destroy},
    .on_event = on_event,
    .memory_needed = 8192, .kind = 2, .flags = 0,
};

__attribute__((used))
const purr_app_table_t *calculator_entry(void)
{
    return &s_table.base;
}
