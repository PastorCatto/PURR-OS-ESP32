/*
 * login_module.c: `whoami`, `id`, `su`, `passwd`, `useradd`, `userdel`, `usermod`, `logout` --
 * the accounts cluster, the third real slice ported onto the prototype kernel table
 * (purr_kernel_table.h), after mem/uptime and the filesystem sweep. Unlike the filesystem
 * calls, every command here except whoami/id is security-sensitive (passwords, the shadow
 * file, admin checks), so each is one opaque, cli-aware call -- the same shape as
 * netinstall's net_install and fs_format (Modules/SPEC.md, PurrOS/components/coreos/SPEC.md
 * section 4.1): all I/O and all real permission/shadow-file logic happen kernel-side. This
 * module is just argument parsing and dispatch.
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

static int cmd_whoami(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    purr_klogin_who_t who;
    s_k->login_whoami(&who);
    s_k->printf(cli, "%s%s\n", who.name, who.is_root ? " (root)" : "");
    return 0;
}

static int cmd_id(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    purr_klogin_who_t who;
    s_k->login_whoami(&who);
    s_k->printf(cli, "uid=%u(%s) role=%s%s\n", (unsigned)who.uid, who.name,
               who.role == PURR_ROLE_USER_ADMIN ? "admin" : "standard",
               who.is_root ? " root" : "");
    return 0;
}

static int cmd_logout(purr_cli_t *cli, int argc, char **argv)
{
    (void)cli; (void)argc; (void)argv;
    s_k->login_logout();
    return 0;
}

static int cmd_su(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    return s_k->login_su(cli);
}

static int cmd_passwd(purr_cli_t *cli, int argc, char **argv)
{
    return s_k->login_passwd(cli, argc > 1 ? argv[1] : NULL);
}

static int cmd_useradd(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 2) {
        s_k->puts(cli, "usage: useradd <name> [admin|standard]\n");
        return 1;
    }
    if (argc > 2 && !streq(argv[2], "admin") && !streq(argv[2], "standard")) {
        s_k->puts(cli, "usage: useradd <name> [admin|standard]\n");
        return 1;
    }
    int as_admin = argc > 2 && streq(argv[2], "admin");
    return s_k->login_useradd(cli, argv[1], as_admin);
}

static int cmd_userdel(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 2) {
        s_k->puts(cli, "usage: userdel <name>\n");
        return 1;
    }
    return s_k->login_userdel(cli, argv[1]);
}

static int cmd_usermod(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 3 || (!streq(argv[2], "admin") && !streq(argv[2], "standard"))) {
        s_k->puts(cli, "usage: usermod <name> admin|standard\n");
        return 1;
    }
    return s_k->login_usermod(cli, argv[1], streq(argv[2], "admin"));
}

static const purr_cmd_t s_cmds[] = {
    {"whoami",  "the logged-in user",            cmd_whoami},
    {"id",      "uid, role and root state",      cmd_id},
    {"su",      "become root (own password)",    cmd_su},
    {"passwd",  "change a password",             cmd_passwd},
    {"useradd", "add an account (admin only)",   cmd_useradd},
    {"userdel", "remove an account (admin only)", cmd_userdel},
    {"usermod", "change a role (admin only)",    cmd_usermod},
    {"logout",  "end the session",               cmd_logout},
};

static const purr_kernel_module_table_t s_table = {
    .abi_version = PURR_KERNEL_TABLE_ABI_VERSION,
    .cmds = s_cmds,
    .cmd_count = 8,
};

__attribute__((used))
const purr_kernel_module_table_t *login_module_entry(const purr_kernel_table_t *kernel)
{
    s_k = kernel;
    return &s_table;
}
