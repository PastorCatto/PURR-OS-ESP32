# Start here

*Current as of PURR OS 1.1 (2026-11-08).*

## Short version

- PURR OS is an operating system for small ESP32-S3 devices (the LilyGO T-Deck Plus and T-Deck; the Waveshare e-paper 1.54 is in progress) with signed images, sandboxed apps and three interchangeable shells.
- Pick the path that matches what you want to do. Each path is a short list of pages, in order.
- Every word you do not know is in the [glossary](glossary.md). Every working procedure is in the [runbook](runbook.md).
- The plans (`plan1.md` to `plan10.md` in the repository root) say what is done and what is not. They are the source of truth: if a page here and a plan disagree, trust the plan and fix the page.

## I want to build and flash it

1. Read [Build directories](runbook.md#build-directories) and install ESP-IDF 5.3.5.
2. Build the three images for your board (kernel, loader, KittenOS): [Make a new build directory](runbook.md#make-a-new-build-directory).
3. Sign and cut slot images: [Sign and assemble](runbook.md#sign-and-assemble). The signing passphrase goes in the environment of the one command and nowhere else.
4. Flash and debug: [Flash and debug a T-Deck Plus](runbook.md#flash-and-debug-a-t-deck-plus).
5. If something fails, read [Traps that have already cost time](runbook.md#traps-that-have-already-cost-time) before anything else.
6. For what each flash region is: [02-flash-map-and-partitions.md](02-flash-map-and-partitions.md).

## I want to write an app

1. Install the developer kit and follow [sdk/docs/00-start.md](../sdk/docs/00-start.md). The kit has everything: Python, the toolchain, `purrstrap`, the `purr` command and PURR Studio.
2. Make a project with `purr new`, build it with `purr build`.
3. Try it without a device: `purr run` puts your app on an emulated device (see [sdk/docs/12-emulator.md](../sdk/docs/12-emulator.md)).
4. Sideload it to a real device through the Developer Portal: [17-sdk.md](17-sdk.md).
5. How apps are loaded, limited and given permissions: [06-app-runtime.md](06-app-runtime.md). The calls your app can make: [05-catcall-abi.md](05-catcall-abi.md).
6. Publishing to other people: [14-store-and-repositories.md](14-store-and-repositories.md).

## I want to understand how it boots

1. [01-boot-chain.md](01-boot-chain.md): bootloader, boot package, KittenOS, loader, kernel, in the order they run.
2. [02-flash-map-and-partitions.md](02-flash-map-and-partitions.md): where each of them sits in flash.
3. [03-trust-and-keys.md](03-trust-and-keys.md): why each one is signed, who may sign what, and what the trust block does.
4. [08-updates-and-release-channels.md](08-updates-and-release-channels.md): how a device gets new images and how a bad one is stopped.
5. [04-kernel-and-services.md](04-kernel-and-services.md): what the kernel does once it is running.

## If you are carrying on without help

The [runbook](runbook.md) was written so that the work can continue from the repository alone. Read it once, from top to bottom, then keep it open. Plans to read, by area: [plan1.md](../plan1.md) boot chain, [plan3.md](../plan3.md) hardening and release channels, [plan5.md](../plan5.md) SDK, [plan6.md](../plan6.md) app runtime, [plan7.md](../plan7.md) shells, [plan8.md](../plan8.md) repositories and Store, [plan9.md](../plan9.md) ui and files, [plan10.md](../plan10.md) power.
