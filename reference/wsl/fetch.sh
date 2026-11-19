#!/usr/bin/env bash
# Fetch the reference emulator sources into WSL's native filesystem (/mnt/c builds are slow).
# Dev-only: the reference is a test oracle and never ships in the plugin.
set -euo pipefail
SRC="${KP_SRC:-$HOME/src}"
MAME_REF="${MAME_REF:-profligacy-public-v1}"
mkdir -p "$SRC" && cd "$SRC"
[ -d mame-profligacy ] || git clone --depth 1 --branch "$MAME_REF" https://github.com/joelanders/mame-profligacy.git
[ -d profligacy ] || git clone --depth 1 https://github.com/joelanders/profligacy.git
git -C mame-profligacy log -1 --format='mame-profligacy %H %cs'
git -C profligacy log -1 --format='profligacy      %H %cs'
