@echo off
rem Makes the KP Sim Prophetic MPC plugin from your ROM dumps in the roms folder (README.md, "Make the plugin").
cd /d "%~dp0"
py -3 --version >nul 2>&1
if not errorlevel 1 (
  py -3 tools\make_plugin.py --open %*
) else (
  python --version >nul 2>&1
  if not errorlevel 1 (
    python tools\make_plugin.py --open %*
  ) else (
    echo Python 3 is needed and was not found.
    echo Install it from https://www.python.org/downloads/ ^(tick "Add python.exe to PATH"^),
    echo or in a terminal: winget install Python.Python.3.12
    echo Then double-click Make Plugin.bat again.
  )
)
pause
