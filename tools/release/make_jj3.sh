#!/bin/zsh
# Builds the files for the JumpingJaguar3 release (the official app repository) from the apps in out/apps.
#
#   tools/release/make_jj3.sh [WORKING_FOLDER]       (default: repos/jj3)
#
# The first time it makes the repository's key (it asks for a password: that is the key's password, keep it) and repo.toml. Then it adds every app in
# out/apps and builds the release folder (WORKING_FOLDER/out/JumpingJaguar3): upload every file in it as an asset of the release tag JumpingJaguar3.
# To make the repository "official" (endorsed), run once: purr repo endorse WORKING_FOLDER --key signing_keys/boot.key --key-id 1  (the boot key's password),
# then build again.
set -e
ROOT=${0:A:h:h:h}
cd $ROOT
W=${1:-repos/jj3}
KP=${PURR_PYTHON:-$HOME/.espressif/python_env/idf5.3_py3.9_env/bin/python}
purr() { $KP sdk/purr.py "$@" }
if [[ ! -f $W/repo.toml ]]; then
  purr repo init $W --name "PURR Apps" --maintainer "PastorCatto" --url "https://github.com/PastorCatto/PURR-OS-ESP32" \
       --description "Pre-compiled apps for PURR OS, each with a link to its source."
fi
add() {   # name title description category perms
  [[ -f out/apps/$1.cat ]] || { echo "skipping $1 (not built)"; return; }
  purr repo add $W out/apps/$1.cat --title "$2" --desc "$3" --category "$4" --perms "$5" --source "https://github.com/PastorCatto/PURR-OS-ESP32/tree/main/Apps/$1"
}
add store    "Store"            "Apps, assets and extras from the repositories you trust, with updates." system "install-apps"
add storesvc "Store companion"  "Checks for updates in the background and installs them if you ask." system "install-apps"
add files    "Files"            "Browse, copy, rename and delete the files in your home folder." tools "shared-storage,system-files"
add devices  "Device Manager"   "Find and add hardware, drivers, the developer server and system updates." system "manage-devices"
add notes    "Notes"            "Write and keep notes." tools "shared-storage"
add beacon   "Beacon"           "Wakes the device every 30 seconds: a sample of the power catcall." samples "wake-alarm,keep-awake"
purr repo release $W
purr repo verify $W/out/JumpingJaguar3
