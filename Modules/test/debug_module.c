/*
 * debug_module.c: a background serial console, running from the moment this module loads
 * (not only while a shell command is active) -- so plugging into UART/USB-Serial-JTAG shows
 * a live prompt immediately and typing reaches the device right away, with no need to first
 * navigate whatever the normal display+input path is (slow, unproven, or itself under
 * suspicion -- exactly the situation a brand-new board's display driver is in before it's
 * trusted; built for the Waveshare154 bring-up). Plus `serial write <text>` / `serial read
 * [ms]` as plain one-off shell commands for scripted use.
 *
 * Deliberately a standalone /kernelmods module, not baked into the kernel table's always-
 * present surface: a hardened build simply doesn't plant debug.cat, and the device has
 * neither the background console nor the raw-serial commands at all -- the module system
 * itself is the on/off switch (purr_kernel_table.h's own comment on serial_write/serial_read/
 * task_create).
 */
#include "purr_kernel_table.h"

static const purr_kernel_table_t *s_k;

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int slen(const char *s)
{
    int n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

/* A small, freestanding decimal parser for the optional timeout argument -- no atoi without
 * libc. Anything that isn't a plain non-negative number falls back to DEFAULT_READ_MS. */
#define DEFAULT_READ_MS 2000

static int parse_ms(const char *s)
{
    int n = 0;
    if (!s || !*s) {
        return DEFAULT_READ_MS;
    }
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') {
            return DEFAULT_READ_MS;
        }
        n = n * 10 + (*p - '0');
    }
    return n;
}

#define LINE_MAX 96

static void write_str(const char *s)
{
    s_k->serial_write(s, slen(s));
}

/* Runs for as long as the device is up (purr_kernel_table_t.task_create -- never returns):
 * prints a banner once, then echoes a line at a time and answers the one built-in "ping"
 * command, purely to prove the round trip works from boot with nothing else running yet.
 * Anything more (running real shell commands over this channel) is future work -- a module
 * has no handle on the shell's own command table today, only the kernel table. */
static void serial_console_task(void *arg)
{
    (void)arg;
    write_str("\r\ndebug: serial console ready (try: ping)\r\n> ");

    char line[LINE_MAX];
    int n = 0;
    for (;;) {
        char c;
        int got = s_k->serial_read(&c, 1, 200);
        if (got <= 0) {
            continue;
        }
        if (c == '\r' || c == '\n') {
            write_str("\r\n");
            if (n > 0) {
                line[n] = '\0';
                if (streq(line, "ping")) {
                    write_str("pong\r\n");
                } else {
                    write_str("? ");
                    s_k->serial_write(line, n);
                    write_str("\r\n");
                }
            }
            n = 0;
            write_str("> ");
            continue;
        }
        if ((c == 0x7F || c == 0x08) && n > 0) {       /* backspace/DEL */
            n--;
            write_str("\b \b");
            continue;
        }
        if (n < LINE_MAX - 1 && c >= 0x20 && c < 0x7F) {
            line[n++] = c;
            s_k->serial_write(&c, 1);                   /* local echo */
        }
    }
}

static int cmd_serial(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 2) {
        s_k->puts(cli, "usage: serial write <text> | serial read [ms]\n");
        return 1;
    }
    if (streq(argv[1], "write")) {
        if (argc < 3) {
            s_k->puts(cli, "usage: serial write <text>\n");
            return 1;
        }
        int n = s_k->serial_write(argv[2], slen(argv[2]));
        s_k->serial_write("\n", 1);
        s_k->printf(cli, "wrote %d byte(s)\n", n);
        return n < 0 ? 1 : 0;
    }
    if (streq(argv[1], "read")) {
        int ms = parse_ms(argc > 2 ? argv[2] : NULL);
        char buf[128];
        int n = s_k->serial_read(buf, sizeof(buf) - 1, ms);
        if (n < 0) {
            s_k->puts(cli, "serial read: error\n");
            return 1;
        }
        buf[n] = '\0';
        s_k->printf(cli, "read %d byte(s): %s\n", n, buf);
        return 0;
    }
    s_k->printf(cli, "serial: unknown subcommand '%s'\n", argv[1]);
    return 1;
}

static const purr_cmd_t s_cmds[] = {
    {"serial", "write <text> | read [ms] -- raw UART/USB-Serial-JTAG", cmd_serial},
};

static const purr_kernel_module_table_t s_table = {
    .abi_version = PURR_KERNEL_TABLE_ABI_VERSION,
    .cmds = s_cmds,
    .cmd_count = 1,
};

__attribute__((used))
const purr_kernel_module_table_t *debug_module_entry(const purr_kernel_table_t *kernel)
{
    s_k = kernel;
    kernel->task_create(serial_console_task, "dbgserial", 3072, NULL, 2);
    return &s_table;
}
