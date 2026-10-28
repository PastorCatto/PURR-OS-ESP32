#include "login.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "sdkconfig.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "commands.h"
#include "purr_abi.h"
#include "purr_cfgstore.h"
#include "purr_console.h"
#include "purr_kernel.h"
#include "purr_settings.h"
#include "purr_userstore.h"

static purr_user_list_t s_users;
static purr_shadow_list_t s_shadow;
static purr_user_t s_current;
static int s_is_root;
static int s_elevated;
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

/* PURR_CFGF_UNATTENDED_SETUP (purr_abi.h): set only by run_auto_mode()'s unattended install,
 * read and cleared here. True means first_time_setup() below must not block on a typed
 * username -- nothing summoned anyone to answer one. */
static int unattended_setup_pending(void)
{
    purr_flash_t fl;
    purr_cfg_t cfg;
    if (purr_cfgstore_open(&fl) != 0 || purr_cfg_load(&fl, &cfg, NULL) < 0) {
        return 0;
    }
    if (!purr_cfg_take_flag(&cfg, PURR_CFGF_UNATTENDED_SETUP)) {
        return 0;
    }
    purr_cfg_store(&fl, &cfg);   /* best-effort: worst case this asks again next boot */
    return 1;
}

/* A short, readable hex string from real randomness (esp_fill_random, the same source
 * purr_login_random_salt already uses) -- enough entropy that it is not guessable, short
 * enough to read off a console and type back in. */
static void random_password(char *buf, size_t cap)
{
    uint8_t raw[6];
    esp_fill_random(raw, sizeof(raw));
    size_t n = 0;
    for (size_t i = 0; i < sizeof(raw) && n + 2 < cap; i++) {
        snprintf(buf + n, cap - n, "%02x", raw[i]);
        n += 2;
    }
}

/* No one is known to be present to answer first_time_setup()'s prompts (an unattended
 * recovery install just finished) -- create a clearly-named fallback admin account instead
 * of blocking forever on a typed username that will never arrive, and print its generated
 * password so whoever eventually looks (the console, or its serial log) can actually log in
 * and fix things up properly. */
static void unattended_first_account(void)
{
    char pass[16];
    random_password(pass, sizeof(pass));
    create_account("recovery", pass, PURR_ROLE_USER_ADMIN);
    purr_login_persist();

    purr_user_t *u = purr_user_list_find(&s_users, "recovery");
    s_current = *u;

    put_str("Unattended recovery install: created a fallback admin account.\n");
    put_str("  login: recovery\n");
    put_str("  password: ");
    put_str(pass);
    put_str("\n");
    put_str("Log in and create a real account, or change this one's password.\n");
    memset(pass, 0, sizeof(pass));
}

static void first_time_setup(void)
{
    if (unattended_setup_pending()) {
        unattended_first_account();
        return;
    }
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

static int s_store_loaded;
static uint32_t s_lock_until_ms[PURR_USER_MAX];
static uint8_t s_lock_armed[PURR_USER_MAX];

static void ensure_loaded(void)
{
    if (!s_store_loaded) {
        purr_userstore_load(purr_login_fs(), &s_users, &s_shadow);
        s_store_loaded = 1;
    }
}

int purr_login_has_accounts(void)
{
    ensure_loaded();
    return s_users.count > 0;
}

purr_login_result_t purr_login_try(const char *name, const char *password, uint32_t *wait_s)
{
    ensure_loaded();
    if (wait_s) {
        *wait_s = 0;
    }
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    purr_user_t *u = (name != NULL) ? purr_user_list_find(&s_users, name) : NULL;
    int idx = (u != NULL) ? (int)(u - s_users.users) : -1;

    /* A user with earlier failures starts the first attempt of a boot already waiting, as the text
     * prompt does after the name is typed. */
    if (idx >= 0 && u->fail_count > 0) {
        if (!s_lock_armed[idx]) {
            s_lock_armed[idx] = 1;
            s_lock_until_ms[idx] = now + purr_login_delay_seconds(u->fail_count) * 1000u;
        }
        if ((int32_t)(s_lock_until_ms[idx] - now) > 0) {
            if (wait_s) {
                *wait_s = (s_lock_until_ms[idx] - now + 999) / 1000;
            }
            return PURR_LOGIN_WAIT;
        }
    }

    /* "root" is never a stored account; its login is always refused (Users/SPEC.md). */
    int ok = (u != NULL) && strcmp(name, "root") != 0 && password != NULL &&
             purr_shadow_check(&s_shadow, name, password);
    if (ok) {
        u->fail_count = 0;
        s_lock_armed[idx] = 0;
        purr_login_persist();
        s_current = *u;
        s_is_root = 0;
        return PURR_LOGIN_OK;
    }
    if (u != NULL) {
        u->fail_count++;
        s_lock_armed[idx] = 1;
        s_lock_until_ms[idx] = now + purr_login_delay_seconds(u->fail_count) * 1000u;
        purr_login_persist();
    }
    return PURR_LOGIN_BAD;
}

void purr_login_run(void)
{
    s_store_loaded = 1;
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

/* Recovery's own user: KittenOS starts as "kitten", a standard account that exists only in memory, so recovery is never
 * an unknown or absent user. uid 254 is not an account and never gets a home folder. */
void purr_login_kitten(void)
{
    purr_login_skip("kitten", PURR_ROLE_USER_STANDARD);
    s_current.uid = 254;
    s_elevated = 0;
}

int purr_login_elevated(void) { return s_elevated; }

/* User account control: an administrator proves who they are and this session becomes theirs, for as long as the device stays
 * on (the session is memory only; a restart is a logout). A wrong password, or an account that is not an administrator, leaves
 * the session exactly as it was. */
purr_login_result_t purr_login_elevate(const char *name, const char *password, uint32_t *wait_s)
{
    purr_user_t before = s_current;
    int root_before = s_is_root;
    purr_login_result_t r = purr_login_try(name, password, wait_s);
    if (r == PURR_LOGIN_OK && s_current.role != PURR_ROLE_USER_ADMIN) {
        s_current = before;
        s_is_root = root_before;
        return PURR_LOGIN_BAD;
    }
    if (r == PURR_LOGIN_OK) {
        s_elevated = 1;
    }
    return r;
}


/* ---------------------------------------------------------------- account changes without a prompt */

static int acting_admin(void)
{
    return purr_login_is_root() || purr_login_current()->role == PURR_ROLE_USER_ADMIN;
}

static void set_password_for(const char *name, const char *password)
{
    uint8_t salt[PURR_SALT_LEN];
    purr_login_random_salt(salt);
    purr_shadow_set(purr_login_shadow(), name, password, salt, PURR_PBKDF2_ITERATIONS);
}

static purr_acct_result_t add_account(const char *name, const char *password, int admin)
{
    if (!purr_user_name_valid(name)) {
        return PURR_ACCT_INVALID;
    }
    if (password == NULL || password[0] == '\0') {
        return PURR_ACCT_BADPASS;
    }
    uint8_t uid = purr_user_next_uid(purr_login_users());
    int r = purr_user_list_add(purr_login_users(), name, uid,
                               admin ? PURR_ROLE_USER_ADMIN : PURR_ROLE_USER_STANDARD);
    if (r == PURR_USER_EXISTS) return PURR_ACCT_EXISTS;
    if (r == PURR_USER_FULL) return PURR_ACCT_FULL;
    if (r != PURR_USER_ADDED) return PURR_ACCT_INVALID;
    set_password_for(name, password);
    if (purr_login_persist() != 0) {
        purr_user_list_remove(purr_login_users(), name);
        purr_shadow_list_remove(purr_login_shadow(), name);
        return PURR_ACCT_SAVE;
    }
    return PURR_ACCT_OK;
}

purr_acct_result_t purr_login_account_add(const char *name, const char *password, int admin)
{
    if (!acting_admin()) {
        return PURR_ACCT_DENIED;
    }
    return add_account(name, password, admin);
}

/* The very first account, from the setup wizard: nobody is signed in yet, so there is no administrator to ask. Allowed only while
 * the device has no accounts at all; it is an administrator. Hashes the password: call it from a worker task. */
purr_acct_result_t purr_login_account_add_first(const char *name, const char *password)
{
    if (purr_login_has_accounts()) {
        return PURR_ACCT_DENIED;
    }
    return add_account(name, password, 1);
}

purr_acct_result_t purr_login_account_set_password(const char *name, const char *new_password,
                                                   const char *current_password)
{
    int is_self = strcmp(name, purr_login_current()->name) == 0;
    if (!is_self && !acting_admin()) {
        return PURR_ACCT_DENIED;
    }
    if (purr_user_list_find(purr_login_users(), name) == NULL) {
        return PURR_ACCT_NOUSER;
    }
    if (new_password == NULL || new_password[0] == '\0') {
        return PURR_ACCT_BADPASS;
    }
    if (is_self && !purr_shadow_check(purr_login_shadow(), name, current_password ? current_password : "")) {
        return PURR_ACCT_BADPASS;
    }
    set_password_for(name, new_password);
    return purr_login_persist() == 0 ? PURR_ACCT_OK : PURR_ACCT_SAVE;
}

purr_acct_result_t purr_login_account_remove(const char *name)
{
    if (!acting_admin()) {
        return PURR_ACCT_DENIED;
    }
    if (strcmp(name, purr_login_current()->name) == 0) {
        return PURR_ACCT_SELF;
    }
    if (purr_user_list_find(purr_login_users(), name) == NULL) {
        return PURR_ACCT_NOUSER;
    }
    if (purr_user_is_last_admin(purr_login_users(), name)) {
        return PURR_ACCT_LAST_ADMIN;
    }
    purr_user_list_remove(purr_login_users(), name);
    purr_shadow_list_remove(purr_login_shadow(), name);
    return purr_login_persist() == 0 ? PURR_ACCT_OK : PURR_ACCT_SAVE;
}

purr_acct_result_t purr_login_account_set_admin(const char *name, int admin)
{
    if (!acting_admin()) {
        return PURR_ACCT_DENIED;
    }
    purr_user_t *u = purr_user_list_find(purr_login_users(), name);
    if (u == NULL) {
        return PURR_ACCT_NOUSER;
    }
    if (!admin && purr_user_is_last_admin(purr_login_users(), name)) {
        return PURR_ACCT_LAST_ADMIN;
    }
    u->role = admin ? PURR_ROLE_USER_ADMIN : PURR_ROLE_USER_STANDARD;
    return purr_login_persist() == 0 ? PURR_ACCT_OK : PURR_ACCT_SAVE;
}

const char *purr_login_account_error(purr_acct_result_t r)
{
    switch (r) {
    case PURR_ACCT_OK:         return "done";
    case PURR_ACCT_DENIED:     return "only an administrator can do that";
    case PURR_ACCT_INVALID:    return "names use letters, digits, - and _";
    case PURR_ACCT_EXISTS:     return "that name is taken";
    case PURR_ACCT_FULL:       return "too many accounts";
    case PURR_ACCT_NOUSER:     return "no such account";
    case PURR_ACCT_LAST_ADMIN: return "that is the last administrator";
    case PURR_ACCT_SELF:       return "you are signed in as that account";
    case PURR_ACCT_BADPASS:    return "password empty, or the current one is wrong";
    case PURR_ACCT_SAVE:       return "could not save";
    case PURR_ACCT_DEVMODE:    return "automatic sign-in needs developer mode, active this boot";
    }
    return "failed";
}


/* ---------------------------------------------------------------- automatic sign-in */

#if CONFIG_PURR_TEST_AUTOLOGIN && !CONFIG_PURR_NET_ETH_W5500
#error "PURR_TEST_AUTOLOGIN puts a known password in the image; it is only allowed in the VM flavor"
#endif

static int s_auto_used;

void purr_login_autologin_user(char *out, size_t cap)
{
    if (purr_settings_kv_get("login.autologin", out, cap) != 0) {
        out[0] = '\0';
    }
}

int purr_login_autologin_used(void) { return s_auto_used; }

int purr_login_autologin(void)
{
    /* Only with developer mode active for THIS boot, as the bootloader's handoff reports it. A secure
     * device, or one whose signed trust block locks developer mode out, therefore never signs in on its
     * own, whatever a file says. */
    int dev = purr_security_dev_active();
    ESP_LOGI("login", "automatic sign-in check: developer mode active this boot = %d", dev);
    if (!dev) {
        return 0;
    }
    ensure_loaded();
    char want[PURR_USER_NAME_LEN];
    purr_login_autologin_user(want, sizeof(want));
    purr_user_t *u = (want[0] != '\0' && strcmp(want, "root") != 0) ? purr_user_list_find(&s_users, want) : NULL;
#if CONFIG_PURR_TEST_AUTOLOGIN
    if (u == NULL) {
        u = purr_user_list_find(&s_users, CONFIG_PURR_TEST_AUTOLOGIN_USER);
        if (u == NULL) {
            create_account(CONFIG_PURR_TEST_AUTOLOGIN_USER, CONFIG_PURR_TEST_AUTOLOGIN_PASSWORD, PURR_ROLE_USER_ADMIN);
            purr_login_persist();
            u = purr_user_list_find(&s_users, CONFIG_PURR_TEST_AUTOLOGIN_USER);
        }
    }
#endif
    if (u == NULL) {
        return 0;
    }
    s_current = *u;
    s_is_root = 0;
    s_auto_used = 1;
    ESP_LOGW("login", "AUTOMATIC SIGN-IN as %s (no password asked)", u->name);
    return 1;
}

purr_acct_result_t purr_login_set_autologin(const char *name, const char *admin_password)
{
    if (!acting_admin()) {
        return PURR_ACCT_DENIED;
    }
    if (name != NULL) {
        if (!purr_security_dev_active()) {
            return PURR_ACCT_DEVMODE;                   /* turning it on needs developer mode, active now */
        }
        if (strcmp(name, "root") == 0 || purr_user_list_find(purr_login_users(), name) == NULL) {
            return PURR_ACCT_NOUSER;
        }
        if (admin_password != NULL && !purr_login_is_root() &&
            !purr_shadow_check(purr_login_shadow(), purr_login_current()->name, admin_password)) {
            return PURR_ACCT_BADPASS;                   /* turning it ON needs the administrator's password */
        }
    }
    return purr_settings_kv_set("login.autologin", name ? name : "") == 0 ? PURR_ACCT_OK : PURR_ACCT_SAVE;
}
