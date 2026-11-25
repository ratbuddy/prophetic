# Releasing Prophetic

The plugin compiles code and data derived from the user's ROMs, so a built plugin can never be
published. A release is the source: users build their own plugin with `tools/make_plugin.py`.
Open MPC lists such plugins as "Build it yourself" (no download; the catalog shows the build
command). The catalog hosts nothing: it is one JSON file per plugin pointing at the author's GitHub
repo, and reads versions and files from that repo's tags and releases. Rules: https://open-mpc.github.io/workflow.html, "Plugins that can't ship a zip", and the
catalog README in open-mpc/mpc-vst-plugins.

## Checklist

| # | Step | How | State |
|---|---|---|---|
| 1 | Open-source licence, root `LICENSE` | GPL-3.0-or-later; third-party parts keep theirs (README, "Licence") | done |
| 2 | Name free of trademarks | "Prophetic", manufacturer "KP Sim" on the MPC; catalog `author` "ratbuddy"; uid `KpPr` unchanged, so saved projects still load | done |
| 3 | One-command build from the dumps | `tools/make_plugin.py`, `Make Plugin.bat`; tested from the release zip (zipped, byte-swapped dumps) | done |
| 4 | ROM audit clean | `py tools/audit_no_rom.py` reports `audit: 0 hit` | before every commit |
| 5 | Source zip | `py tools/make_release.py` -> `out/release/kp-sim-prophetic-<version>.zip` | per release |
| 6 | Device test of a plugin built from that zip | load on a track, play, every page and Q-Link, save and reload a project | per release; 0.1.0: MPC Live III, MPC OS 3.10.1, 2026-11-18 (the release-zip build's `.so` files are byte-identical to the tested ones); 0.1.1: MPC Live III, MPC OS 3.10.1, 2026-11-22: loaded on a track, played, Voices changes, about 3 minutes without a glitch, not every page / Q-Link or a project save and reload (the release-zip build's `.so` files are byte-identical to the ones run) |
| 7 | Version | `plugins/Prophecy/plugin.json` `version`, X.Y.Z: Z fixes, Y new parameters or pages, X only if parameter positions change (breaks projects) | per release |
| 8 | Public GitHub repo with a `vX.Y.Z` tag | https://github.com/ratbuddy/prophetic; the tag must contain `tools/make_plugin.py` and `LICENSE`; tags are never moved once the catalog lists them | per release; v0.1.0 done (moved once, 2026-11-19, before the listing: the address-list fix, row 10); v0.1.1 done |
| 9 | Attach the source zip to the GitHub release of the tag | `kp-sim-prophetic-<version>.zip` from step 5: the only download with the SDK submodule in it (GitHub's own "Source code" zips leave submodules out). Never attach a built plugin: the catalog's nightly check flags any `*-mpc-armv7.zip` as a licence risk | per release; v0.1.0, v0.1.1 done |
| 10 | Git history | the dev history has factory program names in one commit message and in 13 earlier file versions (gone from HEAD); no ROM bytes or tables in any of its 382 blobs. Published as a fresh history (one root commit); the dev history stays local on branch `dev-history`, never pushed. Re-published 2026-11-19 as one root commit again, so the earlier `romdata/` address lists (stretches of ROM pointer tables, [H8.md](H8.md) "Address lists") are in no public commit; the plugin built from it is byte-identical to the tested one | done |
| 11 | `tested.json` at the repo root | one entry per device test, as above, read from the default branch; `id` kp-sim-prophetic; a Gen2 (64-bit) test needs "Gen2" in `device`, else the catalog counts it as Gen1. Without an entry for the newest release the listing loses its Verified badge | per release; 0.1.0, 0.1.1 done |
| 12 | Catalog listing | pull request to open-mpc/mpc-vst-plugins adding only `catalog/plugins/kp-sim-prophetic.json` (below); its PR check runs `tools/catalog_build.py --check-registry` | merged 2026-11-21: open-mpc/mpc-vst-plugins#1015 (later versions need no PR: the nightly build reads the newest release); review round 1: `author` ratbuddy, "bit-exact" out of the summary (it held for the DSP board against the reference emulator, not for the whole instrument or against hardware), the `romdata/` address lists packed so no stretch of them reproduces a ROM table ([H8.md](H8.md), "Address lists") |

## Catalog entry

`catalog/plugins/kp-sim-prophetic.json` for the repository https://github.com/ratbuddy/prophetic.
No versions or checksums: the catalog reads the tags.

```json
{
  "id": "kp-sim-prophetic",
  "name": "Prophetic",
  "author": "ratbuddy",
  "repo": "ratbuddy/prophetic",
  "kind": "instrument",
  "license": "GPL-3.0-or-later",
  "summary": "The Korg Prophecy's synthesis, built on your computer from your own Prophecy v2.0 ROMs: their DSP programs and sound-engine firmware, translated to C++. 128 programs, every parameter, Performance Editor, arpeggiator, 1-8 voices. Gen2; tested on an MPC Live III.",
  "distribution": "build-yourself",
  "requires_user_files": [
    { "name": "Prophecy v2.0 ic12 dump (512 KiB)", "description": "Your own dump of the V55PI ROM, SHA-1 950baf1f6c54394e780898634b7776e49f43acce." },
    { "name": "Prophecy v2.0 ic22 dump (512 KiB)", "description": "Your own dump of the H8/3003 ROM, SHA-1 cd4cb221da19dd6f746dc7f3c145b1494993f6d8." }
  ],
  "build": {
    "command": "python3 tools/make_plugin.py <folder or .zip with your dumps>",
    "script": "tools/make_plugin.py",
    "docs_url": "https://github.com/ratbuddy/prophetic/blob/{tag}/README.md#make-the-plugin-rom-owners",
    "needs": ["Python 3.9+", "Internet once (the compiler download)"]
  },
  "style": "synth",
  "tags": ["emulation", "firmware-derived", "physical-modelling", "arpeggiator", "polyphonic", "gen2", "korg"],
  "components": [
    { "id": "kp-sim-prophetic", "name": "Prophetic", "kind": "instrument", "uid": "KpPr" }
  ]
}
```

| Field | Rule (catalog spec) |
|---|---|
| `id` | lowercase, hyphens; equals the file name; never changes |
| `license` | an open-source SPDX id (`source_available` is not allowed for build-yourself) |
| `build.command` | shown verbatim; must contain `build.script` |
| `build.script` | must exist at the tag, or the tag is not listed |
| `summary` | one line, plain ASCII; only the testing actually done |
| name, skin, screenshot | no manufacturer name as the title, no logos or front-panel art |
