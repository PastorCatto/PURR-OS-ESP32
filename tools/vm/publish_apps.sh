#!/bin/zsh
# Build the sample apps from Apps/<name>/, sign them with the developer key (id 2) and publish them to the dev
# repository folder that `purrstrap dev serve` serves. The passphrase comes from the environment of this one command.
#
#   PURRSTRAP_KEY_PASSWORD=... tools/vm/publish_apps.sh [app ...]      (default: notes beacon)
#
# Then:  python3 purrstrap/purrstrap.py dev serve --dir out/devrepo --port 47801 --name "Claude's Mac"
set -e
ROOT=${0:A:h:h:h}
cd $ROOT
. ~/esp/esp-idf-v5.3.5/export.sh >/dev/null 2>&1
REPO=$ROOT/out/devrepo
APPS=$ROOT/out/apps
mkdir -p $REPO $APPS
ps() { python3 purrstrap/purrstrap.py "$@"; }
names=("$@"); (( $#names )) || names=(notes beacon)
for a in $names; do
  v=$(grep -m1 '^version' Apps/$a/manifest.toml | cut -d'"' -f2)
  t=$(grep -m1 '^description' Apps/$a/manifest.toml | cut -d'"' -f2)
  ps modules build --board tdeck_plus --source Apps/$a/$a.c --entry ${a}_entry --kind app --name $a --version $v --out $APPS/$a.cat | tail -1
  ps keys sign --key signing_keys/developer.key --image $APPS/$a.cat --key-id 2 >/dev/null
  ps dev publish --file $APPS/$a.cat --dir $REPO --title "${(C)a}" --desc "$t" | tail -1
done
