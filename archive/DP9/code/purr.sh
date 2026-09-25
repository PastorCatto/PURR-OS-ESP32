#!/usr/bin/env bash
# purr.sh — PURR OS build system launcher (bash wrapper)
#
# Usage:
#   ./purr.sh                    # interactive tool picker
#   ./purr.sh purrstrap          # purrstrap interactive UI
#   ./purr.sh modulestrap        # modulestrap interactive UI
#   ./purr.sh catstrap           # catstrap interactive UI
#   ./purr.sh purrstrap build tdeck_plus   # pass-through CLI
#
# This script just ensures we run from the repo root and hands off to purr.py.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Prefer python3, fall back to python
if command -v python3 &>/dev/null; then
    PYTHON=python3
elif command -v python &>/dev/null; then
    PYTHON=python
else
    echo "[err] Python not found. Install Python 3.8+ and try again." >&2
    exit 1
fi

exec "$PYTHON" "$SCRIPT_DIR/purr.py" "$@"
