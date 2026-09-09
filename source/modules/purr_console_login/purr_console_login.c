// purr_console_login.c — see purr_console_login.h for the full picture.
#include <string.h>
#include <stdio.h>
#include "purr_console_login.h"
#include "user_mgr.h"
#include "app_manager.h"

void purr_console_login_default_login_fn(const purr_console_io_t *io)
{
    char username[USER_MGR_USERNAME_MAX];

    for (;;) {
        if (user_mgr_count() > 1) {
            purr_console_println("Accounts:");
            for (int i = 0; i < user_mgr_count(); i++) {
                char name[USER_MGR_USERNAME_MAX];
                if (user_mgr_at(i, name, sizeof(name))) {
                    char line[USER_MGR_USERNAME_MAX + 4];
                    snprintf(line, sizeof(line), "  %s", name);
                    purr_console_println(line);
                }
            }
            purr_console_print("login: ");
            purr_console_read_line(io, username, sizeof(username));
            if (!user_mgr_exists(username)) {
                purr_console_println("no such account");
                continue;
            }
        } else {
            // Single-account device — same "don't make someone type a
            // name that's the only possible answer" shortcut
            // purr_systemui_boot_login_check() already takes.
            const char *def = user_mgr_default_username();
            strncpy(username, def ? def : "", sizeof(username) - 1);
            username[sizeof(username) - 1] = '\0';
        }

        if (!user_mgr_has_password(username)) {
            // No-password account — zero-friction auto-login, the exact
            // contract user_mgr.h's own header comment documents (mirrors
            // a real Unix account with an empty/locked shadow entry).
            user_mgr_set_logged_in(username);
            app_manager_notify_unlocked();
            return;
        }

        purr_console_print("password: ");
        char password[64];
        purr_console_set_echo(false);
        purr_console_read_line(io, password, sizeof(password));
        purr_console_set_echo(true);
        bool ok = user_mgr_verify(username, password);
        // Same "don't keep a plaintext secret in RAM longer than it has
        // to be" instinct systemui_login.c's own ctx->password memset
        // already follows.
        memset(password, 0, sizeof(password));
        if (ok) {
            user_mgr_set_logged_in(username);
            app_manager_notify_unlocked();
            return;
        }
        purr_console_println("Login incorrect");
    }
}

void purr_console_login_default_exec_fn(const char *args)
{
    if (!args || !*args) { purr_console_println("exec: missing app name"); return; }
    int rc = app_manager_launch_by_name(args);
    char buf[64];
    snprintf(buf, sizeof(buf), "exec %s: %s", args, rc == 0 ? "launched" : "failed");
    purr_console_println(buf);
}
