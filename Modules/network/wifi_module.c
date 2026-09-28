/*
 * wifi_module.c: `wifi` (scan/connect/forget/list/status) and `net` (bare status) -- the
 * second real subsystem ported out of the monolith and into a loaded module, and the first
 * dependency of netinstall becoming one too. See Modules/SPEC.md for the mechanism, and
 * PurrOS/components/kernel/include/purr_net.h for what this wraps (indirectly -- through the
 * core table's net_* entries, never purr_net.h directly: a module gets no ESP-IDF linkage).
 */
#include "purr_module_abi.h"

static const purr_core_table_t *s_core;

/* A module is -nostdlib/-fno-builtin: no real strcmp to link against, and the compiler is
 * explicitly told not to synthesize one either. A plain local one, matching the project's
 * usual "no libc assumed" style (purr_util.c does the same for coreos). */
static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int cmd_net(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    purr_module_net_status_t st;
    s_core->net_status(&st);
    if (st.connected) {
        s_core->printf(cli, "wifi:    %s (%d dBm)\n", st.ssid, st.rssi);
        s_core->printf(cli, "ip:      %s\n", st.ip);
    } else {
        s_core->puts(cli, "wifi:    not connected\n");
    }
    return 0;
}

static int cmd_wifi_scan(purr_cli_t *cli)
{
    s_core->puts(cli, "scanning...\n");
    purr_module_net_ap_t aps[16];
    int n = 0;
    char err[96];
    if (!s_core->net_scan(aps, 16, &n, err, sizeof(err))) {
        s_core->printf(cli, "%s\n", err);
        return 1;
    }
    int shown = n > 16 ? 16 : n;
    for (int i = 0; i < shown; i++) {
        s_core->printf(cli, "  %-32s %4d dBm  %s\n", aps[i].ssid, aps[i].rssi,
                       aps[i].open ? "open" : "secured");
    }
    if (n > shown) {
        s_core->printf(cli, "  (%d more not shown)\n", n - shown);
    }
    return 0;
}

static int cmd_wifi_connect(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 2) {
        s_core->puts(cli, "usage: wifi connect <ssid> [password]\n");
        return 1;
    }
    const char *pass = argc > 2 ? argv[2] : "";
    s_core->printf(cli, "connecting to %s...\n", argv[1]);
    char err[96];
    if (!s_core->net_connect(argv[1], pass, err, sizeof(err))) {
        s_core->printf(cli, "%s\n", err);
        return 1;
    }
    purr_module_net_status_t st;
    s_core->net_status(&st);
    s_core->printf(cli, "connected, ip %s\n", st.ip);
    return 0;
}

static int cmd_wifi_forget(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 2) {
        s_core->puts(cli, "usage: wifi forget <ssid>\n");
        return 1;
    }
    s_core->net_forget(argv[1]);
    s_core->puts(cli, "forgotten (if it was saved)\n");
    return 0;
}

static int cmd_wifi_list(purr_cli_t *cli)
{
    purr_module_net_ap_t saved[16];
    int n = s_core->net_saved(saved, 16);
    if (n == 0) {
        s_core->puts(cli, "no saved networks\n");
        return 0;
    }
    for (int i = 0; i < n; i++) {
        s_core->printf(cli, "  %-32s %s\n", saved[i].ssid, saved[i].open ? "(open)" : "");
    }
    return 0;
}

static int cmd_wifi(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 2) {
        s_core->puts(cli, "usage: wifi scan|connect|forget|list|status\n");
        return 1;
    }
    if (streq(argv[1], "scan")) return cmd_wifi_scan(cli);
    if (streq(argv[1], "connect")) return cmd_wifi_connect(cli, argc - 1, argv + 1);
    if (streq(argv[1], "forget")) return cmd_wifi_forget(cli, argc - 1, argv + 1);
    if (streq(argv[1], "list")) return cmd_wifi_list(cli);
    if (streq(argv[1], "status")) return cmd_net(cli, 0, NULL);
    s_core->printf(cli, "wifi: unknown subcommand '%s'\n", argv[1]);
    return 1;
}

static const purr_cmd_t s_cmds[] = {
    {"wifi", "scan|connect|forget|list|status", cmd_wifi},
    {"net",  "connection status",                cmd_net},
};

static const purr_module_table_t s_table = {
    .abi_version = PURR_MODULE_ABI_VERSION,
    .cmds = s_cmds,
    .cmd_count = 2,
};

__attribute__((used))
const purr_module_table_t *wifi_module_entry(const purr_core_table_t *core)
{
    s_core = core;
    return &s_table;
}
