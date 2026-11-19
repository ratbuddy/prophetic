# Build the Windows test app (MSVC, x64, static runtime) into out\win\prophecy_win.exe.
# Needs: Visual Studio 2022 (C++ tools), Python (py) and your ROMs in out\win\data (ic22.bin,
# ic12.bin). From them, when missing or with -Regen: the H8 control code generated\h8ctl\h8_gen.cpp
# (tools/h8dec.py, then tools/h8xlat.py) and the DSP programs compiled ahead of time into
# generated\dspaot\dsp_aot_gen.inc (dsp_aot_gen.exe, which also boots the firmware natively and
# writes the engine snapshot state.bin + ram.bin when out\win\data has none). No emulator.
# Run from anywhere: powershell -File apps\win\build.ps1 [-Regen]
param([switch]$Regen)
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
$out = "$repo\out\win"
$obj = "$out\obj"
New-Item -ItemType Directory -Force -Path $obj | Out-Null

$gen = "$out\gen\cpu\tms57002"
New-Item -ItemType Directory -Force -Path $gen | Out-Null
& py "$repo\engine\third_party\tms57002\tmsmake.py" s "$repo\engine\third_party\tms57002\tmsinstr.lst" "$gen\tms57002.hxx"
if ($LASTEXITCODE -ne 0) { throw "tmsmake failed" }
$tp = "$repo\engine\third_party\tms57002"
& py "$repo\tools\tms_regops.py" "$gen\tms57002.hxx" "$tp\tms57002.cpp" "$tp\tms57002.h" "$gen\tms57002_regops.hxx"
if ($LASTEXITCODE -ne 0) { throw "tms_regops failed" }

# 0. H8 control code, translated from your ic22 (korgpro20: the entry and seed address lists in romdata\).
$h8gen = "$repo\generated\h8ctl\h8_gen.cpp"
if ($Regen -or -not (Test-Path $h8gen)) {
	New-Item -ItemType Directory -Force -Path "$repo\generated\h8ctl" | Out-Null
	$ir = "$repo\generated\h8ctl\ic22.ir"
	foreach ($kind in "seeds", "entries") {   # their pointer-table lines are read from your ic22
		& py "$repo\tools\h8_addrlist.py" expand "$out\data\ic22.bin" "$repo\romdata\korgpro20_h8_$kind.txt" -o "$repo\generated\h8ctl\h8_$kind.txt"
		if ($LASTEXITCODE -ne 0) { throw "h8_addrlist failed" }
	}
	& py "$repo\tools\h8dec.py" "$out\data\ic22.bin" --entries "$repo\generated\h8ctl\h8_seeds.txt" -o $ir
	if ($LASTEXITCODE -ne 0) { throw "h8dec failed" }
	& py "$repo\tools\h8xlat.py" $ir $h8gen "$repo\generated\h8ctl\h8_entries.txt"
	if ($LASTEXITCODE -ne 0) { throw "h8xlat failed" }
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio C++ tools not found" }
$vcvars = "$vs\VC\Auxiliary\Build\vcvars64.bat"

# The TMS57002 core is one translation unit (engine\src\dsp_core.cpp includes the vendored .cpp files).
$engine = @(
	"$repo\engine\src\prophecy_board.cpp",
	"$repo\engine\src\control\h8ctl.cpp",
	"$repo\engine\src\control\params.cpp",
	"$repo\engine\src\control\perf.cpp",
	"$repo\engine\src\control\sysex.cpp",
	"$repo\engine\src\synth.cpp",
	"$repo\engine\src\arp.cpp",
	"$repo\generated\h8ctl\h8_gen.cpp"
)
$inc = "/I`"$repo\engine\mame_shim`" /I`"$repo\engine\third_party\tms57002`" /I`"$out\gen`" /I`"$repo\engine\src`" /I`"$repo\generated\dspaot`""
$flags = "/nologo /std:c++17 /O2 /EHsc /MT /W0 /bigobj /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS"

# 1. DSP ahead-of-time compiler (core without generated programs), run on your engine data.
$aot = "$repo\generated\dspaot\dsp_aot_gen.inc"
if ($Regen -or -not (Test-Path $aot)) {
	$gobj = "$out\obj_gen"
	New-Item -ItemType Directory -Force -Path $gobj | Out-Null
	New-Item -ItemType Directory -Force -Path (Split-Path $aot) | Out-Null
	$gsrc = (@("$repo\engine\src\dsp_core.cpp", "$repo\engine\tools\dsp_aot_gen.cpp") + $engine) -join '" "'
	$cmd = "`"$vcvars`" >nul && cl $flags /DKP_DSP_AOT_NONE $inc /Fo`"$gobj\\`" /Fe`"$out\dsp_aot_gen.exe`" `"$gsrc`""
	cmd /c $cmd
	if ($LASTEXITCODE -ne 0) { throw "dsp_aot_gen build failed" }
	& "$out\dsp_aot_gen.exe" "$out\data" $aot
	if ($LASTEXITCODE -ne 0) { throw "dsp_aot_gen failed" }
}

# 2. The app.
$src = (@("$repo\engine\src\dsp_core.cpp") + $engine + @("$repo\apps\win\main.cpp")) -join '" "'
$cmd = "`"$vcvars`" >nul && cl $flags $inc /Fo`"$obj\\`" /Fe`"$out\prophecy_win.exe`" `"$src`" /link /SUBSYSTEM:WINDOWS winmm.lib user32.lib gdi32.lib"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "build failed" }
Write-Host "built $out\prophecy_win.exe"
