# One-step setup on Windows. Run it by double-clicking Setup.cmd; the README's
# "Step by step" is the same thing by hand.
#
# It copies the title's program folder and its sample library into
# game\<title>\ (nothing is downloaded, and nothing leaves this machine),
# builds build-windows\ejay2.exe - which runs the title's own DLLs as they are,
# so there is nothing to lift on Windows - and leaves "<Title> (recomp).cmd".
# Each step is skipped when its output is already there (-Force redoes them).
#
# -Game is an installed program folder, a disc, a folder holding the .iso, or
# the .iso; without it every disc drive is searched. -Yes installs what is
# missing without asking. RECOMP_LAUNCHER=1 (the recomp launcher) skips the
# "Press Enter" at the end. Everything it runs goes to setup.log.
param([switch]$Force, [switch]$Yes, [Alias('GameDir')][string]$Game = "")

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Log = Join-Path $Root 'setup.log'
Set-Location $Root
$Steps = 5
function Log($t) { try { Add-Content -Path $Log -Value $t -Encoding UTF8 -ErrorAction Stop } catch {} }
Log "==== setup $(Get-Date -Format s)"
function Say($t, $c = 'Gray') { Write-Host $t -ForegroundColor $c; Log $t }
function Pause-End { if (-not $env:RECOMP_LAUNCHER) { Read-Host "Press Enter to close" | Out-Null } }
function Step($n, $t) { Write-Host ""; Say "[$n/$Steps] $t" 'Cyan' }
$Mounted = $null
function Fail($t) {
  if ($Mounted) { Dismount-DiskImage -ImagePath $Mounted -ErrorAction SilentlyContinue | Out-Null }
  Say ""; Say "Setup stopped: $t" 'Red'
  Say "The details are in $Log. Fix that and run Setup.cmd again; finished steps are skipped." 'Yellow'
  Pause-End; exit 1
}
function Ask($q) { if ($Yes -or $env:RECOMP_LAUNCHER) { Say "$q yes"; return $true }; $a = Read-Host "$q [Y/n]"; return -not ($a -match '^[nN]') }
function Exec([string[]]$cmd) {
  $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
  $rest = @($cmd | Select-Object -Skip 1)
  & $cmd[0] @rest 2>&1 | ForEach-Object { Log "$_" }
  $code = $LASTEXITCODE
  $ErrorActionPreference = $old
  return $code
}
function Run($what, [string[]]$cmd) { Say "  $what..."; $code = Exec $cmd; if ($code -ne 0) { Fail "$what failed (exit code $code)." } }

Clear-Host
Say "eJay 2 - static recompilation setup" 'White'
Say "You need an eJay 2 disc (or its .iso, or the installed program and the disc's PXD folder) and about 2 GB free."

# ------------------------------------------------------------------ tools
Step 1 "Checking the tools"
# The host is 32-bit C built with Visual Studio's compiler; host\build.bat finds it.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
function Find-VS { if (Test-Path $vswhere) { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath } }
$vs = Find-VS
if (-not $vs) {
  Say "  Visual Studio's C++ build tools are not installed."
  if (-not (Ask "  Install the Visual Studio 2022 Build Tools with the C++ workload now (winget, about 3 GB, asks for admin)?")) {
    Fail "Visual Studio 2022 (or its Build Tools) with 'Desktop development with C++' is needed. Install it, then run Setup.cmd again."
  }
  if (-not (Get-Command winget -ErrorAction SilentlyContinue)) { Fail "winget is missing: install 'App Installer' from the Microsoft Store, or the Build Tools yourself." }
  Exec @('winget', 'install', '-e', '--id', 'Microsoft.VisualStudio.2022.BuildTools', '--accept-package-agreements', '--accept-source-agreements',
         '--override', '--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended') | Out-Null
  $vs = Find-VS
  if (-not $vs) { Fail "the Visual Studio Build Tools did not install. Install them with 'Desktop development with C++', then run Setup.cmd again." }
}
Say "  Visual Studio: $vs"

# ------------------------------------------------------------------ the title
Step 2 "Finding your eJay 2"
# engine DLL -> id, name, palette. Dance eJay 3 (pxd32d5) draws but its mixer
# never arms, so it is not offered yet (ROADMAP).
$Titles = @{
  'pxd32d4.dll' = @{ Id = 'dance2'; Name = 'Dance eJay 2'; Pal = 'DANCE2' }
  'pxd32h4.dll' = @{ Id = 'hiphop2'; Name = 'HipHop eJay 2'; Pal = 'hh2' }
  'pxd32r4.dll' = @{ Id = 'techno'; Name = 'Techno eJay'; Pal = 'RAVE2' }
  'pxd32d5.dll' = @{ Id = 'dance3' }
}
# The program folder under $dir: the one with PXD32CL1.DLL and an engine beside it.
function Find-Program($dir) {
  foreach ($f in @(Get-ChildItem -Path $dir -Recurse -Depth 3 -Filter 'pxd32cl1.dll' -File -ErrorAction SilentlyContinue)) {
    foreach ($e in @(Get-ChildItem -Path $f.DirectoryName -File)) {
      $t = $Titles[$e.Name.ToLower()]
      if ($t) { return @{ Dir = $f.DirectoryName; Engine = $e.Name; T = $t } }
    }
  }
  return $null
}
# The sample library under $dir: a folder with an .INF whose archive is beside it.
function Find-Library($dir) {
  foreach ($f in @(Get-ChildItem -Path $dir -Recurse -Depth 3 -Filter '*.inf' -File -ErrorAction SilentlyContinue)) {
    if ($f.Name -ieq 'autorun.inf') { continue }
    $archive = Join-Path $f.DirectoryName $f.BaseName
    if ((Test-Path $archive -PathType Leaf) -and (Select-String -Path $f.FullName -Pattern '\[SAMPLES\]' -Quiet)) { return $f.DirectoryName }
  }
  return $null
}
function Disc-Drives { Get-CimInstance Win32_LogicalDisk -Filter 'DriveType=5' -ErrorAction SilentlyContinue | Where-Object { $_.VolumeName } | ForEach-Object { $_.DeviceID + '\' } }
# An .iso is mounted read-only (no admin needed) and let go once copied.
function Open-Iso($iso) {
  Say "  Mounting $(Split-Path -Leaf $iso)..."
  $script:Mounted = (Resolve-Path $iso).Path
  $vol = Mount-DiskImage -ImagePath $script:Mounted -Access ReadOnly -PassThru | Get-Volume
  if (-not $vol.DriveLetter) { Fail "Windows mounted $iso but gave it no drive letter." }
  return "$($vol.DriveLetter):\"
}

$src = $null
if ($Game) {
  if (-not (Test-Path $Game)) { Fail "nothing at $Game" }
  if (Test-Path $Game -PathType Leaf) { $src = $Game }
  else {
    $isos = @(Get-ChildItem -Path $Game -Filter '*.iso' -File -ErrorAction SilentlyContinue)
    $src = if (-not (Find-Program $Game) -and $isos.Count -eq 1) { $isos[0].FullName } else { $Game }
  }
} else {
  foreach ($d in @(Disc-Drives)) { if (Find-Program $d) { $src = $d; break } }
  if (-not $src) { Fail "no eJay 2 disc is in a drive: insert it, or run Setup.cmd -Game <the disc, the installed folder, or the .iso>" }
}
if ($src -match '\.iso$') { $src = Open-Iso $src }
$prog = Find-Program $src
if (-not $prog) { Fail "$src holds no eJay 2 program (no PXD32CL1.DLL with an engine beside it)." }
if ($prog.T.Id -eq 'dance3') { Fail "that is Dance eJay 3, which draws but does not play yet: see ROADMAP.md." }
$T = $prog.T
# The library is on the disc; an install leaves it there, so look beside the
# program, then on the disc this came from, then in every disc drive.
$lib = Find-Library (Split-Path -Parent $prog.Dir)
if (-not $lib) { $lib = Find-Library $src }
if (-not $lib) { foreach ($d in @(Disc-Drives)) { $lib = Find-Library $d; if ($lib) { break } } }
if (-not $lib) { Fail "found $($T.Name), but not its sample library (a PXD folder with an .INF): insert the disc too, or pass the disc with -Game." }
Say "  $($T.Name): $($prog.Dir)"
Say "  its sample library: $lib"

# ------------------------------------------------------------------ copy
Step 3 "Copying $($T.Name) into game\$($T.Id) (about 500 MB, once)"
$GameDir = Join-Path $Root "game\$($T.Id)"
if (-not $Force -and (Test-Path (Join-Path $GameDir 'PXD\.copied'))) { Say "  Already there (skipping)." }
else {
  foreach ($pair in @(@($prog.Dir, 'program', 'the program'), @($lib, 'PXD', 'the sample library'))) {
    $to = Join-Path $GameDir $pair[1]
    if (Test-Path $to) { Remove-Item -Recurse -Force $to }
    # robocopy's exit codes 0-7 are success; 8 and up are failures
    Say "  Copying $($pair[2])..."
    $code = Exec @('robocopy', $pair[0], $to, '/E', '/NFL', '/NDL', '/NJH', '/NJS', '/R:1', '/W:1')
    if ($code -ge 8) { Fail "copying $($pair[2]) failed (robocopy exit code $code)." }
  }
  Get-ChildItem -Path $GameDir -Recurse -File | ForEach-Object { $_.IsReadOnly = $false }   # a disc's files arrive read-only
  Set-Content -Path (Join-Path $GameDir 'PXD\.copied') -Value (Get-Date -Format s)
}
if ($Mounted) { Dismount-DiskImage -ImagePath $Mounted | Out-Null; $Mounted = $null }

# ------------------------------------------------------------------ build
Step 4 "Building build-windows\ejay2.exe (a minute)"
$exe = Join-Path $Root 'build-windows\ejay2.exe'
$newest = (Get-ChildItem (Join-Path $Root 'host') -File | Measure-Object -Property LastWriteTime -Maximum).Maximum
if (-not $Force -and (Test-Path $exe) -and (Get-Item $exe).LastWriteTime -gt $newest) { Say "  Already built (skipping)." }
else { Run "Compiling" @('cmd', '/c', ('call "' + (Join-Path $Root 'host\build.bat') + '"')) }

# ------------------------------------------------------------------ launcher
Step 5 "Making the launcher"
$lnk = Join-Path $Root "$($T.Name) (recomp).cmd"
Set-Content -Path $lnk -Encoding ASCII -Value @(
  '@echo off',
  "rem $($T.Name), recompiled: the workspace with a demo arrangement, until you close the window.",
  "cd /d `"%~dp0game\$($T.Id)\program`"",
  "start `"`" `"%~dp0build-windows\ejay2.exe`" --engine $($prog.Engine) --pal $($T.Pal) --lib ..\PXD --song --ticks 0 %*")
Say "  $lnk"
Write-Host ""
Say "Done. Double-click '$($T.Name) (recomp).cmd' to play." 'Green'
Pause-End
