/*
 * purr_cli.h - the shell engine: a line editor, a tokenizer and a command table.
 *
 * Input comes in one character at a time (purr_cli_feed) and output goes out through a
 * sink function, so it runs the same on a display terminal, a serial port or a test.
 * Commands are plain functions in a table. Pipes come later.
 */
#ifndef PURR_CLI_H
#define PURR_CLI_H

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_CLI_LINE     120
#define PURR_CLI_MAX_ARGS 16

typedef struct purr_cli purr_cli_t;
typedef void (*purr_put_fn)(void *ctx, char c);
typedef int (*purr_cmd_fn)(purr_cli_t *cli, int argc, char **argv);

typedef struct {
    const char *name;
    const char *help;                             /* one line */
    purr_cmd_fn fn;
} purr_cmd_t;

struct purr_cli {
    const purr_cmd_t *cmds;
    int ncmds;
    purr_put_fn put;
    void *put_ctx;
    const char *prompt;
    char line[PURR_CLI_LINE];
    int len;
    void *user;                                   /* for the commands' own use */
};

void purr_cli_init(purr_cli_t *cli, const purr_cmd_t *cmds, int ncmds, purr_put_fn put,
                   void *put_ctx, const char *prompt);

void purr_cli_puts(purr_cli_t *cli, const char *s);
void purr_cli_printf(purr_cli_t *cli, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void purr_cli_prompt(purr_cli_t *cli);            /* prints the prompt */

/*
 * One typed character: enter runs the line, backspace or delete removes one, ctrl-U
 * clears the line, other printable characters are added and echoed. Returns 1 when a
 * line was run (the prompt is printed again), else 0.
 */
int purr_cli_feed(purr_cli_t *cli, char c);

/* Run a line as if typed. Returns the command's result, or 127 if there is no such command. */
int purr_cli_run(purr_cli_t *cli, const char *line);

/* Split a line in place into words. Single and double quotes group. Returns the word
 * count, or -1 if there are too many words or a quote is left open. */
int purr_cli_tokenize(char *line, char **argv, int max);

/* A ready-made "help" command that lists the table. */
int purr_cli_cmd_help(purr_cli_t *cli, int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* PURR_CLI_H */
