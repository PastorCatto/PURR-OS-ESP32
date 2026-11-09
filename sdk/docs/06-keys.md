# Keys and signing

*Current as of PURR OS 1.1 (2026-11-08).*

Every file a device installs carries a **signature**: a small proof that a key the device trusts vouched for it. Change one byte of the file and the proof stops matching. That is how a device knows what it is running was not tampered with.

## The vocabulary

- A **key** is a pair: a **private** key you keep secret (it is encrypted with a password), and a **public** key anyone can have.
- A **role** says what a key is for. A device checks that the role is allowed to sign that kind of file.
- A **key id** (1 to 31) is a short number stored in the file's header, so the device knows which key to check against.

| Role | May sign |
|---|---|
| boot | the bootloader, the boot package, KittenOS, the kernel and CoreOS |
| system | drivers, the app manager, runtimes, official system apps |
| owner | apps that hold the `superuser` permission |
| developer | third-party apps and drivers |
| vendor | a third party's modules, signed with their own key |

## Making and using a key

In Studio, open **keys** and **make the key**. Or:

```
bin/purr keys new --role developer --key-id 4
bin/purr sign thing.cat --key developer --key-id 4
bin/purr inspect thing.cat
```

The password is asked for when it is needed and never written down. For scripts, set `PURRSTRAP_KEY_PASSWORD` for that one command. Never put it on a command line.

**Keep the private key safe.** If you lose the password, the key is gone. If someone else gets the file *and* the password, they can sign as you.

## What a device trusts

A device has a **key bag**: the public keys built into it (this kit knows them: `bin/purr keys list`), plus up to 8 extra keys in its signed **trust block**. Only the boot key can sign a trust block.

So for a device to accept files signed by your key, one of these has to be true:

1. Your key is in its key bag, which means a bootloader built from source with your key in it.
2. Your key is in its trust block, signed by the boot key that device trusts.
3. The device is in **developer mode** and its secure mode is **warn**: it boots anything, with a warning. This is how development boards work.

A device whose trust block says **development locked** (a production device) can never be switched into developer mode. That is on purpose.

## What Studio's inspector tells you

When you drop a file, the **Signature** line is one of:

- *good*: signed by a key I know, and the role may sign this kind of file;
- *signed by a role that may not sign this kind of file*: the signature is real, but a device would refuse it;
- *not signed*: a device in enforce mode refuses it;
- *a key I do not know*: add its public key (`<role>_<id>.pub.bin`) to your keys folder and I will check it.

## The key set since OS 1.0.32

A device built from OS 1.0.32 on carries five keys: **1** (old boot), **2** (old developer), **3** (boot), **4** (developer), **5** (hub, role system). Use `--key-id 4` for apps and drivers you sign with the project's developer key, and `bin/purr keys list` to see what your kit's device knows. The old ids 1 and 2 are removed in a later bootloader (0.6.0). A device older than 1.0.32 knows only ids 1 and 2, so a release you sign for it must use one of those until it has been updated once ([docs/03-trust-and-keys.md](../../docs/03-trust-and-keys.md), "The key rotation"). Keep your own private keys out of any folder you commit, and give the passphrase to one command at a time through `PURRSTRAP_KEY_PASSWORD`.
