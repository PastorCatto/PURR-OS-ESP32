#include "purr_cli.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void purr_cli_init(purr_cli_t *cli, const purr_cmd_t *cmds, int ncmds, purr_put_fn put,
                   void *put_ctx, const char *prompt)
{
    memset(cli, 0, sizeof(*cli));
    cli->cmds = cmds;
    cli->ncmds = ncmds;
    cli->put = put;
    cli->put_ctx = put_ctx;
    cli->prompt = prompt ? prompt : "> ";
}

void purr_cli_puts(purr_cli_t *cli, const char *s)
{
    while (*s) {
        cli->put(cli->put_ctx, *s++);
    }
}

void purr_cli_printf(purr_cli_t *cli, const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    purr_cli_puts(cli, buf);
}

void purr_cli_prompt(purr_cli_t *cli)
{
    purr_cli_puts(cli, cli->prompt);
}

int purr_cli_tokenize(char *line, char **argv, int max)
{
    int argc = 0;
    char *p = line;
    for (;;) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            return argc;
        }
        if (argc >= max) {
            return -1;
        }
        argv[argc++] = p;
        char *out = p;                            /* the word is rewritten in place */
        char quote = 0;
        while (*p) {
            if (quote) {
                if (*p == quote) {
                    quote = 0;
                    p++;
                } else {
                    *out++ = *p++;
                }
            } else if (*p == '\'' || *p == '"') {
                quote = *p++;
            } else if (*p == ' ' || *p == '\t') {
                break;
            } else {
                *out++ = *p++;
            }
        }
        if (quote) {
            return -1;
        }
        if (*p) {
            p++;                                  /* skip the space that ended the word */
        }
        *out = '\0';
    }
}

int purr_cli_run(purr_cli_t *cli, const char *line)
{
    char buf[PURR_CLI_LINE];
    char *argv[PURR_CLI_MAX_ARGS];
    size_t n = strlen(line);
    if (n >= sizeof(buf)) {
        purr_cli_puts(cli, "line too long\n");
        return 1;
    }
    memcpy(buf, line, n + 1);

    int argc = purr_cli_tokenize(buf, argv, PURR_CLI_MAX_ARGS);
    if (argc < 0) {
        purr_cli_puts(cli, "syntax error: too many words or an open quote\n");
        return 1;
    }
    if (argc == 0) {
        return 0;
    }
    for (int i = 0; i < cli->ncmds; i++) {
        if (strcmp(cli->cmds[i].name, argv[0]) == 0) {
            return cli->cmds[i].fn(cli, argc, argv);
        }
    }
    purr_cli_printf(cli, "%s: command not found (try help)\n", argv[0]);
    return 127;
}

int purr_cli_feed(purr_cli_t *cli, char c)
{
    if (c == '\r' || c == '\n') {
        cli->put(cli->put_ctx, '\n');
        cli->line[cli->len] = '\0';
        cli->len = 0;
        purr_cli_run(cli, cli->line);
        purr_cli_prompt(cli);
        return 1;
    }
    if (c == '\b' || c == 0x7F) {
        if (cli->len > 0) {
            cli->len--;
            purr_cli_puts(cli, "\b \b");
        }
        return 0;
    }
    if (c == 0x15) {                              /* ctrl-U */
        while (cli->len > 0) {
            cli->len--;
            purr_cli_puts(cli, "\b \b");
        }
        return 0;
    }
    if (c >= 0x20 && c <= 0x7E && cli->len < PURR_CLI_LINE - 1) {
        cli->line[cli->len++] = c;
        cli->put(cli->put_ctx, c);
    }
    return 0;
}

int purr_cli_cmd_help(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    for (int i = 0; i < cli->ncmds; i++) {
        purr_cli_printf(cli, "  %-8s %s\n", cli->cmds[i].name, cli->cmds[i].help);
    }
    return 0;
}
