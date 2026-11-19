# Provenance

| Item | Value |
|---|---|
| Source | https://github.com/joelanders/mame-profligacy, branch `profligacy-public-v1` |
| Commit | see `.upstream_rev` (eb19791d0286f5de5492ac0303b776af475e2e29, 2026-09-10) |
| Path | `src/devices/cpu/tms57002/` |
| Files | `tms57002.cpp`, `tms57002.h`, `tms57kdec.cpp`, `tmsops.cpp`, `tmsinstr.lst`, `tmsmake.py` |
| License | BSD-3-Clause (per-file headers; copyright-holders: Olivier Galibert), text in `LICENSE.BSD-3-Clause` |
| Modifications | None. Compiled against `engine/mame_shim/` (our stand-ins for `emu.h`, the disassembler and the JIT) |
| Generated | `tms57002.hxx` is produced at build time: `python3 tmsmake.py s tmsinstr.lst <out>` |

Contains no Korg firmware or firmware-derived data.
