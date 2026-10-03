#include "login.h"

#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "commands.h"
#include "purr_console.h"
#include "purr_kernel.h"
#include "purr_userstore.h"

static purr_user_list_t s_users;
static purr_shadow_list_t s_shadow;
static purr_user_t s_current;
static int s_is_root;
static int s_logout_requested;

purr_user_list_t *purr_login_users(void) { return &s_users; }
purr_shadow_list_t *purr_login_shadow(void) { return &s_shadow; }
const purr_user_t *purr_login_current(void) { return &s_current; }
int purr_login_is_root(void) { return s_is_root; }
void purr_login_set_root(int is_root) { s_is_root = is_root; }
void purr_login_request_logout(void) { s_logout_requested = 1; }

int purr_login_take_logout(void)
{
    int r = s_logout_requested;
    s_logout_requested = 0;
    return r;
}

void purr_login_random_salt(uint8_t salt[PURR_SALT_LEN])
{
    esp_fill_random(salt, PURR_SALT_LEN);
}

int purr_login_persist(void)
{
    return purr_userstore_save(purr_login_fs(), &s_users, &s_shadow);
}

/* ---------------------------------------------------------------- console I/O */

static void put_str(const char *s)
{
    while (*s) {
        purr_console_put(NULL, *s++);
    }
    purr_console_flush();
}

static void read_line(char *buf, size_t cap, int mask)
{
    size_t len = 0;
    for (;;) {
        char c = purr_kernel_key();
        if (c == 0) {
            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }
        if (c == '\r' || c == '\n') {
            break;
        }
        if ((c == '\b' || c == 0x7F) && len > 0) {
            len--;
            put_str("\b \b");
            continue;
        }
        if (c >= 0x20 && c <= 0x7E && len < cap - 1) {
            buf[len++] = c;
            char echo[2] = {(char)(mask ? '*' : c), 0};
            put_str(echo);
        }
    }
    buf[len] = '\0';
    put_str("\n");
}

/* ---------------------------------------------------------------- setup and login */

static void create_account(const char *name, const char *password, purr_user_role_t role)
{
    uint8_t uid = purr_user_next_uid(&s_users);
    purr_user_list_add(&s_users, name, uid, role);
    uint8_t salt[PURR_SALT_LEN];
    purr_login_random_salt(salt);
    purr_shadow_set(&s_shadow, name, password, salt, PURR_PBKDF2_ITERATIONS);
}

static void first_time_setup(void)
{
    put_str("No accounts yet. Let's create the first one (an admin).\n");
    char name[PURR_USER_NAME_LEN], pass[64], confirm[64];
    for (;;) {
        put_str("Name: ");
        read_line(name, sizeof(name), 0);
        /* F-11: letters, digits, '-', '_' only, not "root" -- that account's login is
         * always refused, so creating it here would lock the device out until someone
         * wiped it from the PC by hand. purr_user_list_add() would already have refused
         * it (PURR_USER_INVALID), but checking here means a bad name never reaches
         * create_account() at all, not just that it would have been rejected silently. */
        if (purr_user_name_valid(name)) {
            break;
        }
        put_str("That name will not work. Try again.\n");
    }
    for (;;) {
        put_str("Password: ");
        read_line(pass, sizeof(pass), 1);
        put_str("Confirm password: ");
        read_line(confirm, sizeof(confirm), 1);
        if (strcmp(pass, confirm) == 0 && pass[0] != '\0') {
            break;
        }
        put_str("Those did not match, or were empty. Try again.\n");
    }
    memset(confirm, 0, sizeof(confirm));

    create_account(name, pass, PURR_ROLE_USER_ADMIN);
    memset(pass, 0, sizeof(pass));
    purr_login_persist();

    purr_user_t *u = purr_user_list_find(&s_users, name);
    s_current = *u;
    put_str("Account created. You are logged in.\n");
}

static void login_prompt(void)
{
    for (;;) {
        char name[PURR_USER_NAME_LEN], pass[64];
        put_str("login: ");
        read_line(name, sizeof(name), 0);
        purr_user_t *u = purr_user_list_find(&s_users, name);

        if (u != NULL && u->fail_count > 0) {
            uint32_t wait = purr_login_delay_seconds(u->fail_count);
            char msg[48];
            snprintf(msg, sizeof(msg), "waiting %u second%s...\n", (unsigned)wait, wait == 1 ? "" : "s");
            put_str(msg);
            vTaskDelay(pdMS_TO_TICKS(wait * 1000));
        }

        put_str("password: ");
        read_line(pass, sizeof(pass), 1);

        /* "root" is never a stored account; its login is always refused (Users/SPEC.md). */
        int ok = (u != NULL) && strcmp(name, "root") != 0 && purr_shadow_check(&s_shadow, name, pass);
        memset(pass, 0, sizeof(pass));

        if (ok) {
            u->fail_count = 0;
            purr_login_persist();
            s_current = *u;
            s_is_root = 0;
            put_str("\n");
            return;
        }
        if (u != NULL) {
            u->fail_count++;
            purr_login_persist();
        }
        put_str("login incorrect\n\n");
    }
}

void purr_login_run(void)
{
    purr_userstore_load(purr_login_fs(), &s_users, &s_shadow);
    if (s_users.count == 0) {
        first_time_setup();
    } else {
        login_prompt();
    }
}

void purr_login_skip(const char *name, purr_user_role_t role)
{
    purr_userstore_load(purr_login_fs(), &s_users, &s_shadow);
    memset(&s_current, 0, sizeof(s_current));
    strncpy(s_current.name, name, sizeof(s_current.name) - 1);
    s_current.role = role;
    s_is_root = 0;
}
