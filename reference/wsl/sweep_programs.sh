#!/usr/bin/env bash
# Run capture_program.sh for programs 0..127 in parallel on WSL's native disk, then summarise
# into data/traces/sweep_<set>/ (unique program images + summary.json; firmware-derived, gitignored).
# Usage: sweep_programs.sh [set=korgpro20] [jobs=nproc-2] [first=0] [last=127]
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SET="${1:-korgpro20}"
JOBS="${2:-$(( $(nproc) - 2 ))}"
FIRST="${3:-0}"; LAST="${4:-127}"
ROOT="${KP_TRACE_ROOT:-$HOME/kp-traces}/sweep_$SET"
mkdir -p "$ROOT"
seq "$FIRST" "$LAST" | xargs -P "$JOBS" -I{} bash "$REPO/reference/wsl/capture_program.sh" "$SET" {} "$ROOT"
python3 -I "$REPO/tools/sweep_summary.py" "$ROOT" "$REPO/data/traces/sweep_$SET"
