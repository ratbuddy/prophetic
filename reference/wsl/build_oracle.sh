#!/usr/bin/env bash
# Build the headless MAME 'propmin' subtarget (Prophecy driver only) from the Profligacy fork.
# Requires: build-essential pkg-config libsdl2-dev libsdl2-ttf-dev libfontconfig-dev libxinerama-dev libxi-dev
set -euo pipefail
MAME_DIR="${MAME_DIR:-$HOME/src/mame-profligacy}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PATCH_DIR="$REPO/reference/patches"

cd "$MAME_DIR"
# Apply our tracing patches (idempotent: skip any already applied).
if compgen -G "$PATCH_DIR/*.patch" > /dev/null; then
	for p in "$PATCH_DIR"/*.patch; do
		if git apply --check "$p" 2>/dev/null; then
			git apply "$p" && echo "applied $(basename "$p")"
		elif git apply --reverse --check "$p" 2>/dev/null; then
			echo "already applied $(basename "$p")"
		else
			echo "patch does not apply: $p" >&2; exit 1
		fi
	done
fi

JOBS="${JOBS:-$(nproc)}" NOWERROR="${NOWERROR:-1}" ./scripts/korgprophecy_build.sh \
	NO_USE_PULSEAUDIO=1 NO_USE_PIPEWIRE=1 "$@"
ls -la "$MAME_DIR/propmin"
