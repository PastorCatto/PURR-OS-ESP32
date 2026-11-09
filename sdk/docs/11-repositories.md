# Running a repository

*Current as of PURR OS 1.1 (2026-11-08).*

A repository is a folder of files on any web host (a GitHub release is perfect) that lists apps, assets and extras, and holds them. People add it to their device, and the Store shows what is in it. **Whoever runs a repository signs it.** The full format is in `Repositories/SPEC.md`; this is how to do it.

## Make one

```
bin/purr repo init myrepo --name "My Apps" --maintainer "me" --url "https://github.com/me/my-apps"
```

It asks for a password and makes a key for the repository (`myrepo/keys/repo.key`, protected by that password) and a `repo.toml`. **The key is your repository's identity.** Anyone who adds your repository trusts this key; if you lose it, you have to start a new repository, and if it leaks, someone else can publish as you. Keep `keys/` out of git (it already is in `.gitignore`).

## Add things

```
bin/purr repo add myrepo build/clock.cat --title "Clock" --desc "Tells the time" --category tools --source https://github.com/me/clock --perms keep-awake
bin/purr repo add myrepo wallpaper.bin --type asset --name sunset --version 1.0 --dest wallpapers/sunset.bin --target any --title "Sunset wallpaper"
```

- An **app** is a `.cat` (built with `purr build`). It may be signed by you, by the app's author, or not at all: your repository's signature on the index is what vouches for it. The Store shows people the permissions it asks for (`--perms`).
- An **asset** or **extra** is any file. `--dest` is where it goes under the item's folder (no `..`). `--target flash`, `sd` or `any`; add `--needs-sd` in the metadata if it cannot work without a card.

## Build and publish

```
bin/purr repo build myrepo            # flat: for a GitHub release; add --layout tree for a plain web host
bin/purr repo verify myrepo/out
```

`build` writes `repo.json`, `index.json`, a signature file for each, and the packages. **Upload every file** in `myrepo/out` as the assets of one GitHub release (flat layout), or to your web host (tree layout). `verify` checks a folder or an `https://` address the way a device does, so run it on the real address after you upload.

People add it with the address of the release, like `https://github.com/me/my-apps/releases/download/v1`. The Store shows the name, your name and the **fingerprint** (the id, in groups of four letters) and asks them to confirm. Publish the fingerprint somewhere people can compare it (the README of the repository is a good place).

## Trust, in short

- **official**: the project endorsed your key (only the project can).
- **community**: people added it and confirmed your fingerprint. This is the normal case.
- **untrusted**: something does not match (a bad signature, or a changed key). It works only with developer mode on, and the Store says so.

If you ever change the key, it is a different repository: everyone has to add it again and confirm the new fingerprint. That is on purpose.

## Moving the official update address (project only)

`purr repo move OUTFOLDER --from-url <the recovery.manifest devices ask now> --recovery <the new one> --modules <the new modules.manifest> --seq 1 --key <the boot key> [--message "..."]` writes `moved.txt` and `moved.txt.sig`. Put both next to the manifest the devices ask. A device follows it only if the boot key signed it, `from` is the address it asked, `seq` is higher than the last move it followed, and the new addresses are https and end in `/recovery.manifest` and `/modules.manifest`. This needs the boot key, so only the project can do it; it has nothing to do with your own repository. How it works: [docs/08](../../docs/08-updates-and-release-channels.md).

## Items, subfolders and the project's hub

- An item has a type: `app`, `asset`, `extra`, `theme` (a `pack.bin` up to 4 MB) or `shell` (one signed `shell.cat` up to 1 MB). `purr repo add --type theme` and `--type shell` check these rules.
- The index entry's `url` may be a path with `/` (for instance `apps/clock-1.0.0.cat`) as long as it is at most 95 characters, with no leading `/` or `.`, no `..` and no `//`. A device joins it to the repository's address, so you can keep packages in subfolders; `repo.json`, `index.json` and their signatures stay at the root.
- A device knows a repository by the fingerprint of its key, so **one key is one repository**: two folders signed with the same key would be the same repository on a device.
- A device keeps at most 32 items per repository, 48 in the Store and 24 KB of index, and drops a whole entry whose title (31), description (79), category (15) or version (15, no underscore) is too long. `purr repo verify` reports these.
- The project's own repository is the hub, `PURR-OS-HUB`, built with its own small tool (`tools/hubsign.py` there) that makes the same bytes as `purr repo build`. If you want your own store, fork it, make your key with `hubsign.py keygen`, and delete `endorse_key_id` from `repo.toml`: your repository is then "community", not "official" ([docs/14-store-and-repositories.md](../../docs/14-store-and-repositories.md)).
