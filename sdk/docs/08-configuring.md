# Configuring a device

*Current as of PURR OS 1.1 (2026-11-08).*

Three files, and the boot trust block, decide how a device behaves. Studio's **configure** tool edits them with checks, so you cannot write a value the device would refuse.

## settings (`/etc/system.conf`)

One `key=value` per line. The keys are the settings the shell registers (the `settings` command on the device lists them). Studio knows each one's help and what it accepts, for example:

```
display.brightness=128
power.screen_timeout=60
theme.accent=Teal
time.clock24=on
```

A setting you leave out keeps its default. Settings marked **admin** affect everyone on the device.

## apps (`/etc/apps.conf`)

How the app runtime behaves, in Hyprland style, `keyword = value`:

```
general:max_apps = 8
general:reserve_kb = 64
rule = background, name:sync
exec-boot = sync
exec-once = notes
```

- `general:` keys are numbers: how many apps at once, how much memory to keep for the system, when to tell apps to trim, and when to stop the least important ones.
- `rule` lines say which apps may keep running in the background.
- `exec-boot` apps start with nobody logged in; `exec-once` apps start after the first sign-in of a boot.

## the boot trust block

The small signed area that says how strict a device is: **enforce** (refuse anything unsigned or altered), **warn** (boot it with a warning) or **off**; whether development is locked out for good; and a few one-shot flags for test devices. Only the boot key can sign it, so Studio asks for the boot key and its password and writes a `purrcfg.bin` you can flash at the trust-block partition.

## the board profile (`/etc/board.conf`)

Which extra parts a device has. Write it from a board file: see [Describing a board](03-boards.md).
