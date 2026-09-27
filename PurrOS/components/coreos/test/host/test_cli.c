#include <string.h>

#include "purr_cli.h"
#include "purr_term.h"
#include "testkit.h"

/* ---------------------------------------------------------------- terminal */

static void test_term_basics(void)
{
    purr_term_t t;
    purr_term_init(&t, 10, 3);
    purr_term_puts(&t, "hello");
    CHECK(strncmp(t.cell[0], "hello     ", 10) == 0);
    CHECK_EQ(t.cx, 5);
    CHECK_EQ(t.cy, 0);

    purr_term_putc(&t, '\n');
    CHECK_EQ(t.cx, 0);
    CHECK_EQ(t.cy, 1);

    purr_term_puts(&t, "ab");
    purr_term_putc(&t, '\b');
    purr_term_putc(&t, ' ');
    purr_term_putc(&t, '\b');                     /* the editor's erase: back, blank, back */
    CHECK_EQ(t.cx, 1);
    CHECK_EQ(t.cell[1][1], ' ');

    purr_term_putc(&t, '\b');
    purr_term_putc(&t, '\b');                     /* never left of column 0 */
    CHECK_EQ(t.cx, 0);
}

static void test_term_wrap_and_scroll(void)
{
    purr_term_t t;
    purr_term_init(&t, 4, 2);
    purr_term_puts(&t, "abcdef");                 /* wraps after four */
    CHECK(strncmp(t.cell[0], "abcd", 4) == 0);
    CHECK(strncmp(t.cell[1], "ef  ", 4) == 0);
    CHECK_EQ(t.cy, 1);

    purr_term_puts(&t, "\nxyz");                  /* row 1 full of text, so this scrolls */
    CHECK(strncmp(t.cell[0], "ef  ", 4) == 0);
    CHECK(strncmp(t.cell[1], "xyz ", 4) == 0);
    CHECK_EQ(t.cy, 1);
}

static void test_term_dirty(void)
{
    purr_term_t t;
    purr_term_init(&t, 8, 3);
    for (int r = 0; r < 3; r++) {
        CHECK(purr_term_take_dirty(&t, r));       /* a new terminal is all dirty */
        CHECK(!purr_term_take_dirty(&t, r));      /* and taking clears it */
    }
    purr_term_putc(&t, 'x');
    CHECK(purr_term_take_dirty(&t, 0));
    CHECK(!purr_term_take_dirty(&t, 1));
    CHECK(!purr_term_take_dirty(&t, -1));
    CHECK(!purr_term_take_dirty(&t, 99));
}

static void test_term_limits(void)
{
    purr_term_t t;
    purr_term_init(&t, 9999, 9999);
    CHECK_EQ(t.cols, PURR_TERM_MAX_COLS);
    CHECK_EQ(t.rows, PURR_TERM_MAX_ROWS);
    purr_term_init(&t, 0, -5);
    CHECK_EQ(t.cols, 1);
    CHECK_EQ(t.rows, 1);
    purr_term_putc(&t, 0x01);                     /* control characters are ignored */
    purr_term_putc(&t, (char)0xC3);
    CHECK_EQ(t.cx, 0);
}

/* ---------------------------------------------------------------- tokenizer */

static int tok(const char *in, char **argv, char *buf, size_t n)
{
    strncpy(buf, in, n);
    buf[n - 1] = '\0';
    return purr_cli_tokenize(buf, argv, PURR_CLI_MAX_ARGS);
}

static void test_tokenize(void)
{
    char buf[128], *argv[PURR_CLI_MAX_ARGS];

    CHECK_EQ(tok("", argv, buf, sizeof(buf)), 0);
    CHECK_EQ(tok("   \t ", argv, buf, sizeof(buf)), 0);

    CHECK_EQ(tok("ls -l  /boot", argv, buf, sizeof(buf)), 3);
    CHECK(strcmp(argv[0], "ls") == 0);
    CHECK(strcmp(argv[1], "-l") == 0);
    CHECK(strcmp(argv[2], "/boot") == 0);

    CHECK_EQ(tok("  echo   hi  ", argv, buf, sizeof(buf)), 2);
    CHECK(strcmp(argv[1], "hi") == 0);

    CHECK_EQ(tok("echo \"a b\" 'c d'", argv, buf, sizeof(buf)), 3);
    CHECK(strcmp(argv[1], "a b") == 0);
    CHECK(strcmp(argv[2], "c d") == 0);

    CHECK_EQ(tok("echo a\"b c\"d", argv, buf, sizeof(buf)), 2);   /* quotes inside a word */
    CHECK(strcmp(argv[1], "ab cd") == 0);

    CHECK_EQ(tok("echo \"\"", argv, buf, sizeof(buf)), 2);        /* an empty word is a word */
    CHECK(strcmp(argv[1], "") == 0);

    CHECK_EQ(tok("echo \"open", argv, buf, sizeof(buf)), -1);
    CHECK_EQ(tok("echo 'open", argv, buf, sizeof(buf)), -1);
    CHECK_EQ(tok("echo \"it's\"", argv, buf, sizeof(buf)), 2);    /* the other quote is plain */
    CHECK(strcmp(argv[1], "it's") == 0);

    CHECK_EQ(tok("1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16", argv, buf, sizeof(buf)), 16);
    CHECK_EQ(tok("1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17", argv, buf, sizeof(buf)), -1);
}

/* ---------------------------------------------------------------- shell */

static char s_out[1024];
static int s_out_len;
static int s_calls, s_argc;
static char s_first[32];

static void sink(void *ctx, char c)
{
    (void)ctx;
    if (s_out_len < (int)sizeof(s_out) - 1) {
        s_out[s_out_len++] = c;
        s_out[s_out_len] = '\0';
    }
}

static int cmd_echo(purr_cli_t *cli, int argc, char **argv)
{
    s_calls++;
    s_argc = argc;
    snprintf(s_first, sizeof(s_first), "%s", argc > 1 ? argv[1] : "");
    for (int i = 1; i < argc; i++) {
        purr_cli_printf(cli, "%s%s", i > 1 ? " " : "", argv[i]);
    }
    purr_cli_puts(cli, "\n");
    return 0;
}

static int cmd_fail(purr_cli_t *cli, int argc, char **argv)
{
    (void)cli; (void)argc; (void)argv;
    return 3;
}

static const purr_cmd_t cmds[] = {
    {"help", "list the commands", purr_cli_cmd_help},
    {"echo", "print the arguments", cmd_echo},
    {"fail", "always returns 3", cmd_fail},
};

static void reset(purr_cli_t *cli)
{
    s_out[0] = '\0';
    s_out_len = 0;
    s_calls = 0;
    purr_cli_init(cli, cmds, 3, sink, NULL, "$ ");
}

static void type(purr_cli_t *cli, const char *s)
{
    while (*s) {
        purr_cli_feed(cli, *s++);
    }
}

static void test_run(void)
{
    purr_cli_t cli;
    reset(&cli);
    CHECK_EQ(purr_cli_run(&cli, "echo hello world"), 0);
    CHECK(strcmp(s_out, "hello world\n") == 0);
    CHECK_EQ(s_argc, 3);

    CHECK_EQ(purr_cli_run(&cli, "fail"), 3);      /* the command's result comes back */
    CHECK_EQ(purr_cli_run(&cli, "   "), 0);       /* a blank line does nothing */
    CHECK_EQ(s_calls, 1);

    s_out_len = 0; s_out[0] = '\0';
    CHECK_EQ(purr_cli_run(&cli, "nope"), 127);
    CHECK(strstr(s_out, "nope: command not found") != NULL);

    s_out_len = 0; s_out[0] = '\0';
    CHECK_EQ(purr_cli_run(&cli, "echo \"open"), 1);
    CHECK(strstr(s_out, "syntax error") != NULL);

    char longline[400];
    memset(longline, 'a', sizeof(longline) - 1);
    longline[sizeof(longline) - 1] = '\0';
    s_out_len = 0; s_out[0] = '\0';
    CHECK_EQ(purr_cli_run(&cli, longline), 1);
    CHECK(strstr(s_out, "too long") != NULL);
}

static void test_help(void)
{
    purr_cli_t cli;
    reset(&cli);
    CHECK_EQ(purr_cli_run(&cli, "help"), 0);
    CHECK(strstr(s_out, "echo") != NULL);
    CHECK(strstr(s_out, "print the arguments") != NULL);
    CHECK(strstr(s_out, "fail") != NULL);
}

static void test_editing(void)
{
    purr_cli_t cli;
    reset(&cli);
    purr_cli_prompt(&cli);
    type(&cli, "echo hi");
    CHECK(strcmp(s_out, "$ echo hi") == 0);       /* typed characters are echoed */
    CHECK_EQ(s_calls, 0);                         /* nothing runs until enter */

    CHECK_EQ(purr_cli_feed(&cli, '\r'), 1);
    CHECK_EQ(s_calls, 1);
    CHECK(strstr(s_out, "hi\n$ ") != NULL);       /* result, then a fresh prompt */

    /* Backspace removes the last character, in the line and on screen. */
    reset(&cli);
    type(&cli, "echo abx");
    purr_cli_feed(&cli, '\b');
    type(&cli, "c");
    CHECK(strstr(s_out, "\b \b") != NULL);
    purr_cli_feed(&cli, '\r');
    CHECK(strcmp(s_first, "abc") == 0);

    /* Delete works the same as backspace. */
    reset(&cli);
    type(&cli, "echo zz");
    purr_cli_feed(&cli, 0x7F);
    purr_cli_feed(&cli, '\n');
    CHECK(strcmp(s_first, "z") == 0);

    /* Backspace on an empty line does nothing. */
    reset(&cli);
    purr_cli_feed(&cli, '\b');
    CHECK_EQ(s_out_len, 0);

    /* Ctrl-U clears the whole line. */
    reset(&cli);
    type(&cli, "echo wrong");
    purr_cli_feed(&cli, 0x15);
    type(&cli, "echo right");
    purr_cli_feed(&cli, '\r');
    CHECK(strcmp(s_first, "right") == 0);
    CHECK_EQ(s_calls, 1);

    /* Control and non-ASCII bytes are ignored. */
    reset(&cli);
    purr_cli_feed(&cli, 0x01);
    purr_cli_feed(&cli, (char)0xC3);
    CHECK_EQ(s_out_len, 0);
}

static void test_line_limit(void)
{
    purr_cli_t cli;
    reset(&cli);
    for (int i = 0; i < PURR_CLI_LINE + 50; i++) {
        purr_cli_feed(&cli, 'a');
    }
    CHECK_EQ(cli.len, PURR_CLI_LINE - 1);         /* extra characters are dropped */
    CHECK_EQ(s_out_len, PURR_CLI_LINE - 1);       /* and not echoed */
    purr_cli_feed(&cli, '\r');                    /* running it is safe */
    CHECK_EQ(cli.len, 0);
}

static void test_into_terminal(void)
{
    /* The shell and the terminal work together: type a command, read the screen. */
    static purr_term_t term;
    purr_term_init(&term, 20, 6);
    purr_cli_t cli;
    purr_cli_init(&cli, cmds, 3, (purr_put_fn)purr_term_putc, &term, "$ ");
    /* purr_term_putc takes (term, char), the sink takes (ctx, char): same shape. */
    purr_cli_prompt(&cli);
    type(&cli, "echo hey");
    purr_cli_feed(&cli, '\r');
    CHECK(strncmp(term.cell[0], "$ echo hey", 10) == 0);
    CHECK(strncmp(term.cell[1], "hey", 3) == 0);
    CHECK(strncmp(term.cell[2], "$ ", 2) == 0);
    CHECK_EQ(term.cy, 2);
    CHECK_EQ(term.cx, 2);
}

int main(void)
{
    test_term_basics();
    test_term_wrap_and_scroll();
    test_term_dirty();
    test_term_limits();
    test_tokenize();
    test_run();
    test_help();
    test_editing();
    test_line_limit();
    test_into_terminal();
    TK_DONE("test_cli");
}
