# Users spec (draft 0.1)

Accounts, login, and how they meet apps and remote access. Permissions are in
`../Permissions/SPEC.md`, the shell is in `../AppManager/SPEC.md` section 8.1 and
`../PurrOS/components/coreos/SPEC.md` section 3.7, and boards are in `../Boards/SPEC.md`.

## 1. Decisions so far

- **Multiple user accounts,** Unix-style. The main reason is that accounts are **the identity for remote
  sessions:** one device logging into another, as Milkbar did with its server login and admin dashboard.
- **Root exists, and its own login is locked.** **The first account created at setup is an admin.** An admin
  **escalates to root with `su`,** using their own password. Every other account is a standard user.
- **Passwords** are stored as a salted, iterated hash, never as text (section 2).
- **Repeated wrong passwords** cause a **growing delay:** 1 second, 2, 4 and so on up to a minute. The
  counter is **saved across reboots,** so power-cycling does not reset it. There is no permanent lockout,
  because anyone with physical access to the flash can bypass a login anyway.
- **Apps are installed once for the whole device.** There is one copy in `/apps`. **Each user has their own
  data and their own permission grants:** app data under `/home/<user>`, and a separate record of what that
  user allowed. One user allowing GPS to an app does not allow it for the next user.
- **Auto-login is the default on boards with no touch and no keyboard,** only buttons (for example the
  Waveshare e-paper board). It logs in an **ordinary, non-admin user.** It never applies to root, and `su`
  still asks for the password. Admin actions on such a board go through the serial console and `su`.
  Boards with a keyboard (T-Deck Plus) or touch (CYD) show a normal login.
- **Remote access is off by default.** An admin turns on a remote access service, and other devices log in
  with an account over an encrypted connection. Admins get admin abilities remotely, and standard users get
  a standard user's view.
- **On auto-login boards remote access is on by default for the ordinary user,** because remote is how such
  a board is used. It still needs a **password,** set during first setup over the serial console. Auto-login
  only skips the password on the device itself. Remote access for an admin or root there still needs an admin
  to enable it.

## 2. Accounts and passwords

- Accounts live in root-only files under `/etc`: a list of users (name, id, role, home folder) and a separate
  file of password hashes. Root has id 0. Roles are admin and standard. *(Proposed layout.)*
- **The hash** is PBKDF2 with SHA-256, a random salt per user, and an iteration count stored with each hash so
  it can be raised later. The comparison takes constant time. A password never appears in a log, and buffers
  that held one are cleared. *(Proposed.)*
- Without flash encryption, the hashes can be read from the flash by someone with physical access. The salt
  and iterations slow guessing but do not stop it. This is the same limit as the saved Wi-Fi passwords
  (`../Network/SPEC.md` section 1).

## 3. First-time setup

- The first boot creates the first account, an admin.
- On a board with a keyboard, this happens on the device. On a board with touch and no keyboard, it needs an
  on-screen keyboard (`../UI/SPEC.md` section 5), so until then it happens over the serial console.
- On an auto-login board, setup creates the admin **and** the ordinary auto-login user with its password, over
  the serial console.

## 4. Login and the shell

At boot the console shows a login. Commands, with placeholder names: `login`, `logout`, `su`, `passwd`,
`useradd`, `userdel`, `usermod`, `whoami` and `id`. Only admins can add, remove or change other accounts.

## 5. How it meets apps and permissions

- **Apps run as the user who started them.** An app's data folder is under that user's home, so the `storage`
  catcall's root is per user (`../Catcalls/SPEC.md` section 3).
- **Permission grants are per user** (`../Permissions/SPEC.md`).
- **Only admins can install a privileged app or grant `device-admin`.** *(Proposed.)*
- **The owner key is not an account.** It signs code, and is separate from who is logged in
  (`../Keys/SPEC.md`).

## 6. Remote access

**Pinned for later.** Remote access is deferred until a UI has been built. The first builds
focus on the internet recovery core and the basics (`../PurrOS/SPEC.md` section 10).

Only the account side is decided here. The remote protocol, sessions, and what remote control can do (Milkbar's
server login, admin dashboard and remote desktop) are a chunk of their own, still to come. What is decided:
off by default, logins over an encrypted connection, admin and standard users treated differently, and the
auto-login-board exception in section 1.

## 7. Testing

- Password checks: correct, wrong, and timing that does not leak the length or content.
- The delay: grows with each failure, resets after a success, survives a reboot, and is capped.
- Roles: a standard user cannot add users, use `su` or install a privileged app, and an admin can.
- Root's own login is refused.
- Per-user data and grants: two users with one app do not see each other's data or allowed permissions.
- Auto-login: only on boards with no touch and no keyboard, never as an admin, and remote login still needs a
  password.
- First-time setup on each kind of board.

## 8. Open questions

- **The hash iteration count,** tuned to how fast the ESP32 can compute it.
- **The remote access design:** protocol, transport and what a remote session can do.
- **Groups,** and password rules.
- **A forgotten admin password:** the way back in, for example an entry in the boot menu that resets accounts,
  or a reflash.
- **Whether KittenOS asks for a login** before destructive actions such as an update or a restore.
- **Several users logged in at once,** and switching users without logging out.
- **A screen lock.**
- **Per-user storage limits.**
- **The e-paper board showing the logged-in name** on its panel, as the old system did.
