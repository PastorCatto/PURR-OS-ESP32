#!/usr/bin/env bash
# setup_esp_idf.sh — installs ESP-IDF 5.3.5 on Debian/Ubuntu
set -e

IDF_VERSION="v5.3.5"
IDF_PATH="$HOME/esp/esp-idf"
IDF_TARGETS="esp32,esp32s3"

RED='\033[0;31m'
GRN='\033[0;32m'
YLW='\033[1;33m'
NC='\033[0m'

info()  { echo -e "${GRN}[setup]${NC} $*"; }
warn()  { echo -e "${YLW}[warn]${NC}  $*"; }
error() { echo -e "${RED}[error]${NC} $*"; exit 1; }

# ── Prerequisites ─────────────────────────────────────────────────────────────

info "Installing system prerequisites..."
sudo apt-get update -q
sudo apt-get install -y \
    git cmake ninja-build wget flex bison gperf \
    python3 python3-pip python3-venv python3-setuptools \
    libffi-dev libssl-dev dfu-util libusb-1.0-0 \
    ccache

# ── Clone or update ESP-IDF ───────────────────────────────────────────────────

mkdir -p "$HOME/esp"

if [ -d "$IDF_PATH/.git" ]; then
    warn "ESP-IDF already cloned at $IDF_PATH — checking version..."
    current=$(git -C "$IDF_PATH" describe --tags 2>/dev/null || echo "unknown")
    if [ "$current" = "$IDF_VERSION" ]; then
        info "Already at $IDF_VERSION, skipping clone."
    else
        warn "Found $current, re-cloning for $IDF_VERSION..."
        rm -rf "$IDF_PATH"
        git clone --depth 1 --branch "$IDF_VERSION" \
            https://github.com/espressif/esp-idf.git "$IDF_PATH"
        git -C "$IDF_PATH" submodule update --init --recursive
    fi
else
    info "Cloning ESP-IDF $IDF_VERSION to $IDF_PATH..."
    git clone --depth 1 --branch "$IDF_VERSION" \
        https://github.com/espressif/esp-idf.git "$IDF_PATH"
    git -C "$IDF_PATH" submodule update --init --recursive
fi

# ── Install toolchains ────────────────────────────────────────────────────────

info "Installing toolchains for targets: $IDF_TARGETS (this may take a few minutes)..."
"$IDF_PATH/install.sh" $IDF_TARGETS

# ── Shell integration ─────────────────────────────────────────────────────────

EXPORT_LINE=". \"\$HOME/esp/esp-idf/export.sh\" > /dev/null 2>&1"
ALIAS_LINE="alias get_idf='. \"\$HOME/esp/esp-idf/export.sh\"'"

for RC in "$HOME/.bashrc" "$HOME/.zshrc"; do
    [ -f "$RC" ] || continue
    if ! grep -q "esp-idf/export.sh" "$RC"; then
        echo "" >> "$RC"
        echo "# ESP-IDF $IDF_VERSION" >> "$RC"
        echo "$ALIAS_LINE" >> "$RC"
        info "Added get_idf alias to $RC"
    else
        warn "ESP-IDF alias already in $RC — skipping."
    fi
done

# ── Verify ────────────────────────────────────────────────────────────────────

info "Verifying install..."
source "$IDF_PATH/export.sh" > /dev/null 2>&1
idf_ver=$(idf.py --version 2>&1 | head -1)
info "idf.py reports: $idf_ver"

echo ""
echo -e "${GRN}Done!${NC} ESP-IDF $IDF_VERSION is ready."
echo ""
echo "  In new terminals, run:  get_idf"
echo "  Or source directly:     . ~/esp/esp-idf/export.sh"
echo ""
