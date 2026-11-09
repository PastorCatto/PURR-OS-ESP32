# Releases and the dev server

*Current as of PURR OS 1.1 (2026-11-08).*

## A release

A release is the set of parts a device needs to recover itself: the **kernel**, **KittenOS**, the recovery **loader**, the **boot package** and the **bootloader**, each signed, with manifests that list their sizes and hashes.

The kit has the prebuilt parts of the current system in `images/`. In Studio's **release** tool you pick the parts, pick a key, and press **gather it**. Each part is re-signed with your key, then laid out as:

```
release-<name>/
  recovery/
    recovery.manifest
    modules.manifest
    kernel-tdeck_plus.cat
    kittenos-tdeck_plus.kitt
    loader-tdeck_plus.cat
    bootpkg-tdeck_plus.cat
    bootloader-tdeck_plus.cat
```

From a terminal: `bin/purr release --board tdeck_plus --key boot --key-id 1`.

Swap in your flavor's boot package by choosing it as the `bootpkg` part (build the flavor project and sign it).

## Look inside it

Drop the `recovery` folder onto Studio. Every file in the manifest is checked for its size and hash, and every image for its signature.

## The dev server

The dev server hands your files to a device on your local network. It is the same server the `purrstrap dev serve` command runs. Start it in Studio's **server** tool, or:

```
bin/purr serve --dir release-demo
```

It announces itself on the network. A device that is in **developer mode** with **dev recovery** on shows it as a choice in setup and in recovery, so you can install straight from your computer. The files are still checked against the device's keys before anything is written.

Publish a driver or an app to it with the **publish to server** button after a build, or `bin/purr publish thing.cat`.

## Good to know

- The server only listens for devices on your network; there is no internet part. Stop it with the button, or Ctrl+C in the terminal.
- It does not change what a device trusts. Files signed with a key the device does not know are refused (or booted with a warning in warn mode).
