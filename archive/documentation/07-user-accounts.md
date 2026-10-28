# 07. User accounts

PURR OS has Unix-style accounts: a login at boot, admin and standard roles, `su`, per-user home
folders and per-user app installs. Source: `PurrOS/main/login.c`, `PurrOS/components/coreos/src/purr_users.c`,
`PurrOS/components/kernel/src/purr_userstore.c`, and the account section of `PurrOS/main/commands.c`.
Design: `Users/SPEC.md`.

**Status: [WORKS]** for creating accounts, logging in, `su`, `passwd`, role changes and the growing
login delay (97 host checks on the account and password logic, 197 on PBKDF2).
**[PROBLEM]** Authorization is thin: only account commands are admin-gated ([F-10](FINDINGS.md#f-10)).
**[DESIGNED]** and absent: groups, password rules, auto-login boards, remote access, per-user app
permissions, screen lock.

## The model

| Concept | What it is today |
|---------|------------------|
| **Account** | name (up to 31 characters), uid (1 to 255, lowest free number), role, failed-login counter |
| **Roles** | `admin` or `standard` |
| **root** | **Not an account.** uid 0 is never stored and `login` as `root` is always refused. An admin becomes root for the session with `su`. |
| **Limit** | 16 accounts |
| **Password** | stored as PBKDF2-HMAC-SHA256, 10,000 iterations, 16-byte random salt (hardware RNG). The iteration count is stored with each hash so it can be raised later. Compared in constant time. |
| **Session** | one user logged in at a time, in memory only. `logout` returns to the login prompt. No concurrent sessions, no switching without logging out, no screen lock. |

### Where it is stored

Two plain text files on `root`:

`/etc/passwd`, one line per account, tab separated:

```
name<TAB>uid<TAB>role<TAB>fail_count<TAB>next_allowed_time
alice	1	1	0	0
bob	2	0	0	0
```

`role` is `1` for admin, `0` for standard. `next_allowed_time` is stored but never used
([F-12](FINDINGS.md#f-12)).

`/etc/shadow`, one line per account:

```
name<TAB>salt_hex(32 chars)<TAB>iterations<TAB>hash_hex(64 chars)
```

Both files are written whole, atomically per file (LittleFS replaces the old contents only when the
new file is closed). Malformed lines are skipped on load.

**There is no file permission system.** Any logged-in user, standard or admin, can `cat /etc/shadow`,
`write` over `/etc/passwd`, or `rm` either ([F-10](FINDINGS.md#f-10)). The spec calls these root-only
files; nothing enforces it.

## First boot: creating the first account

If `/etc/passwd` has no accounts, the login gate runs first-time setup instead of a login prompt:

```
No accounts yet. Let's create the first one (an admin).
Name: alice
Password: ********
Confirm password: ********
Account created. You are logged in.
```

- The first account is always an **admin**.
- Name: must not be empty or contain a tab. Anything else is accepted, including a name that later
  breaks things: **do not use `root`** (login refuses it, so that account could never log in) and avoid
  `/`, `..` and spaces ([F-11](FINDINGS.md#f-11)).
- Password: printable ASCII only (the keyboard path rejects other bytes), up to 63 characters, must be
  non-empty and typed twice identically. No other rules.
- On KittenOS with no PURR OS installed there is no first-time setup: the session is the in-memory user
  `setup` (an admin) and no account file is touched.

## Logging in

```
login: alice
password: ********
```

- `root` is refused.
- A wrong password increments that account's stored `fail_count` and prints `login incorrect`.
- **Growing delay:** the next time that account name is entered, the system waits before asking for the
  password. The wait is 1, 2, 4, 8, 16, 32 then 60 seconds (capped), for 1, 2, 3 ... failures. It prints
  `waiting N second(s)...`. The count is saved to flash, so power-cycling does not reset it. A success
  resets it. There is **no permanent lockout** (anyone with flash access could bypass a login anyway).
- Typing a name that does not exist gives `login incorrect` with no delay, which tells an attacker which
  names exist ([F-12](FINDINGS.md#f-12)).

## Everyday recipes

All commands are typed at the shell prompt ([06](06-using-the-shell.md)).

### Add a standard user

```
alice@PURR OS> useradd bob
set their password:
new password: ********
confirm: ********
bob created (uid 2, standard)
```

Admin only. If the two passwords differ, or are empty, nothing is created.

### Add another admin

```
useradd carol admin
```

Or promote an existing user: `usermod bob admin`. Demote with `usermod bob standard`. The last admin
cannot be demoted (`usermod: refusing to demote the last admin`).

### Change a password

Your own:

```
passwd
current password: ********
new password: ********
confirm: ********
password changed
```

Someone else's (admin or root only, no current-password prompt): `passwd bob`.

### Remove a user

```
userdel bob
```

Admin only. You cannot delete the account you are logged in as, or the last admin. **It does not delete
their `/home/bob` folder or their apps.** Clean those up yourself with `rm`, deepest first
(`rm /home/bob/apps/<app>/package.cat`, `rm /home/bob/apps/<app>`, ... , `rm /home/bob`), or leave them.
If you later create a new `bob`, it inherits the old folder.

### Become root

```
su
password: ********
whoami      -> alice (root)
```

Only admins can `su`; it asks for **your own** password. Root lasts until `logout`. Today "root" only
adds one capability: it can change another account's password without being an admin, which an admin
can already do. Nothing else checks it.

### Log out

`logout`.

### What each role can do, as actually enforced

| Action | standard | admin | note |
|--------|----------|-------|------|
| `passwd` (own) | yes | yes | |
| `passwd other` | no | yes | |
| `useradd`, `userdel`, `usermod` | no | yes | |
| `su` | no | yes | |
| everything else (files, `format`, `netinstall`, `reboot`, `wifi`, `appinstall`, ...) | **yes** | yes | not gated ([F-10](FINDINGS.md#f-10)) |

## Per-user folders and apps

- `/home/<name>/apps` is created at boot for every registered user, and on first `appinstall`.
- **Apps are installed per user**, not once for the device. `apps` and `appinstall` act on the logged-in
  user's own folder, and `su` does not change which one (it stays the admin's own). The spec says one
  shared copy under `/apps` with per-user data and grants; the code differs ([F-09](FINDINGS.md#f-09)).
  See [11](11-apps.md).

## Forgotten password

There is **no reset path**. Options, from gentlest:

1. Another admin runs `passwd <name>`.
2. If the only admin is locked out: wipe `root` from the PC, which deletes **everything** on it
   (accounts, `/system` and `/kernelmods` modules, apps, saved Wi-Fi):

   ```powershell
   python -m esptool --chip esp32s3 --port COM5 erase_region 0x501000 0xAFF000
   ```

   On the next boot KittenOS formats `root` and PURR OS runs first-time setup again. Reinstall modules
   with `netinstall modules` or `plantmodules`, then `reboot` ([10](10-updates-network-install-and-recovery.md)).
3. If the only admin simply cannot be reached, KittenOS shares the same account files, so it does not help
   (unless no PURR OS image is installed, when KittenOS skips login).

The spec lists a boot-menu "reset accounts" entry as an open idea. It does not exist.

## Not built

Auto-login on boards with no keyboard, remote access, groups, password rules, per-user storage
quotas, per-user permission grants, a screen lock and showing the user on an e-paper panel are all in
`Users/SPEC.md` and none exist.

Next: [08 Networking](08-networking.md).
