#!/bin/zsh
# Put a new build of one of the project's own apps into the hub (a checkout of PURR-OS-HUB next to this one): builds Apps/<app>/, signs it with the developer
# key (id 4), writes repos/official/apps/<app>-<version>.cat, sets the version and the commit in repos/official/items/<app>.toml, and removes versions older than the previous one.
# The passphrase comes from PURRSTRAP_KEY_PASSWORD in the environment of this one command. It does NOT sign the hub's index or commit or push anything:
# after it, in the hub, run `python3 tools/hubsign.py build` (hub key) and commit. The app must already have an items/<app>.toml in the hub (first time: write one).
#
#   PURRSTRAP_KEY_PASSWORD=... tools/vm/hub_app.sh messages [devices ...]      (HUB=/path/to/PURR-OS-HUB to override the default ../PURR-OS-HUB)
set -e
ROOT=${0:A:h:h:h}
cd $ROOT
HUB=${HUB:-$ROOT/../PURR-OS-HUB}
KEY=${KEY:-$ROOT/signing_keys/new/developer.key}
KEYID=${KEYID:-4}
[[ -d $HUB/repos/official/apps ]] || { echo "no $HUB/repos/official/apps: set HUB to your PURR-OS-HUB checkout" >&2; exit 1; }
. ~/esp/esp-idf-v5.3.5/export.sh >/dev/null 2>&1
commit=$(git rev-parse HEAD)
git diff --quiet -- Apps || echo "note: Apps/ has uncommitted changes, so the commit written in the item is not exactly what was built" >&2
tmp=$(mktemp -d)
for a in "$@"; do
  item=$HUB/repos/official/items/$a.toml
  [[ -f $item ]] || { echo "$a: no $item (write the item description first)" >&2; exit 1; }
  v=$(grep -m1 '^version' Apps/$a/manifest.toml | cut -d'"' -f2)
  python3 purrstrap/purrstrap.py modules build --board tdeck_plus --source Apps/$a/$a.c --entry ${a}_entry --kind app --name $a --version $v --out $tmp/$a.cat >/dev/null
  python3 purrstrap/purrstrap.py keys sign --key $KEY --image $tmp/$a.cat --key-id $KEYID >/dev/null
  python3 purrstrap/purrstrap.py keys verify --key ${KEY%.key}.pub --image $tmp/$a.cat | tail -1
  cp $tmp/$a.cat $HUB/repos/official/apps/$a-$v.cat
  sed -i '' -e "s/^version = .*/version = \"$v\"/" -e "s/^commit = .*/commit = \"$commit\"/" \
    -e "s#^source = .*#source = \"https://github.com/PastorCatto/PURR-OS-ESP32/tree/$commit/Apps/$a\"#" $item
  # keep the current and the previous version, remove older ones
  python3 - "$HUB/repos/official/apps" "$a" <<'PYEOF'
import glob, os, re, sys
folder, name = sys.argv[1], sys.argv[2]
def key(p):
    return [int(x) if x.isdigit() else x for x in re.split(r"[.+-]", os.path.basename(p)[len(name) + 1:-4])]
for old in sorted(glob.glob(os.path.join(folder, name + "-[0-9]*.cat")), key=key)[:-2]:
    os.remove(old)
    print("removed", old)
PYEOF
  echo "$a $v -> $HUB/repos/official/apps/$a-$v.cat"
done
rm -rf $tmp
echo "next, in the hub: python3 tools/hubsign.py build   (hub key, id 5), then commit"
