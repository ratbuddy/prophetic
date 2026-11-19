Put your own Prophecy v2.0 ROM dumps in this folder:

  ic12 (NEC V55PI, panel/MIDI CPU)    512 KiB
  ic22 (Hitachi H8/3003, voice CPU)   512 KiB

Any file names. Loose .bin files or a .zip that holds them both work, and byte-swapped dumps
(the two bytes of every 16-bit word the other way round) are put right automatically.

Optional: your own Prophecy SysEx files (.syx). They become the plugin's user programs.

Then:
  Windows        double-click "Make Plugin.bat" (one folder up)
  macOS, Linux   python3 tools/make_plugin.py

Nothing in this folder is ever committed or shared (docs/BYO-ROM.md).
