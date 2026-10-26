#!/bin/bash
# One-step setup on Linux and macOS: the same steps as "Step by step" in the
# README, in order, each skipped when its output is already there.
#
#   ./setup.sh                      # find an eJay 2 disc that is mounted
#   ./setup.sh --game-dir DIR       # an installed program folder, a disc, or a folder with the .iso
#   ./setup.sh --game-dir X.iso     # the disc image itself
#   ./setup.sh --yes                # install what is missing without asking (macOS: Homebrew)
#   ./setup.sh --force              # redo every step
#
# It copies the title's program folder and its sample library into
# game/<title>/ (nothing is downloaded, and nothing leaves this machine),
# lifts the title's two DLLs to C, builds build-<title>/ejay2 on pcrecomp's
# win32hle, and leaves "<Title> (recomp).sh" (".command" on a Mac) here.
# Everything it runs is logged to setup.log. RECOMP_LAUNCHER set (the recomp
# launcher) means nobody is there to answer, as --yes does.
set -u
cd "$(dirname "$0")" || exit 1
ROOT=$PWD
LOG=$ROOT/setup.log
YES=0 FORCE=0 GAME=""
[ -n "${RECOMP_LAUNCHER:-}" ] && YES=1
while [ $# -gt 0 ]; do
  case "$1" in
    --yes) YES=1 ;;
    --force) FORCE=1 ;;
    --game-dir) GAME=${2:-}; shift ;;
    *) echo "usage: ./setup.sh [--game-dir DIR|X.iso] [--yes] [--force]" >&2; exit 2 ;;
  esac
  shift
done
echo "==== setup $(date '+%F %T')" >> "$LOG"
STEPS=6
say()  { printf '%s\n' "$*"; printf '%s\n' "$*" >> "$LOG"; }
step() { printf '\n\033[36m[%s/%s] %s\033[0m\n' "$1" "$STEPS" "$2"; printf '[%s/%s] %s\n' "$1" "$STEPS" "$2" >> "$LOG"; }
fail() {
  printf '\n\033[31mSetup stopped: %s\033[0m\n' "$*" >&2; printf 'Setup stopped: %s\n' "$*" >> "$LOG"
  printf 'The details are in %s. Fix that and run ./setup.sh again; finished steps are skipped.\n' "$LOG" >&2
  exit 1
}
ask() { [ "$YES" = 1 ] && { say "$1 yes"; return 0; }; read -r -p "$1 [Y/n] " a; case "$a" in [nN]*) return 1 ;; esac; return 0; }
run() { local what=$1; shift; say "  $what..."; "$@" >> "$LOG" 2>&1 || fail "$what failed: the last lines of $LOG say why."; }
done_already() { [ "$FORCE" = 0 ] && [ -e "$1" ]; }
MAC=0; [ "$(uname -s)" = Darwin ] && MAC=1

say "eJay 2 - static recompilation setup"
say "You need an eJay 2 disc (or its .iso, or the installed program and the disc's PXD folder) and about 2 GB free."

# ------------------------------------------------------------------ tools
step 1 "Checking the tools"
PY=python3
need=()
for t in cc cmake ninja pkg-config; do command -v $t >/dev/null || need+=("$t"); done
pkg-config --exists sdl2 SDL2_ttf 2>/dev/null || need+=("SDL2 and SDL2_ttf")
command -v python3 >/dev/null || need+=("python3")
# win32hle draws Windows' fonts with Liberation (DejaVu at a pinch): fontconfig, or the usual places
font=0
fc-match -f '%{file}' 'Liberation Sans' 2>/dev/null | grep -qiE 'liberation|dejavu' && font=1
for f in /usr/share/fonts/truetype/liberation*/LiberationSans-Regular.ttf /usr/share/fonts/liberation*/LiberationSans-Regular.ttf /usr/share/fonts/TTF/LiberationSans-Regular.ttf; do
  [ -f "$f" ] && font=1
done
[ "$MAC" = 0 ] && [ "$font" = 0 ] && need+=("the Liberation fonts")
if [ ${#need[@]} -gt 0 ]; then
  say "  Missing: ${need[*]}"
  if [ "$MAC" = 1 ]; then
    command -v brew >/dev/null || fail "Homebrew is needed: https://brew.sh, then run ./setup.sh again."
    xcode-select -p >/dev/null 2>&1 || fail "Xcode's command line tools are needed: xcode-select --install, then run ./setup.sh again."
    ask "  Install cmake, ninja, pkgconf, sdl2, sdl2_ttf and python with Homebrew now?" || fail "those are required."
    run "Installing with Homebrew" brew install cmake ninja pkgconf sdl2 sdl2_ttf python
  else
    say "  Debian, Ubuntu: sudo apt install build-essential cmake ninja-build pkg-config libsdl2-dev libsdl2-ttf-dev fonts-liberation python3 python3-venv"
    say "  Fedora:         sudo dnf install gcc cmake ninja-build pkgconf SDL2-devel SDL2_ttf-devel liberation-sans-fonts python3"
    say "  Arch:           sudo pacman -S base-devel cmake ninja pkgconf sdl2 sdl2_ttf ttf-liberation python"
    fail "install those (the line for your system is above), then run ./setup.sh again."
  fi
fi
# The lift's Python packages, in a virtual environment of this folder's own if
# the system's Python lacks them (Debian and Homebrew refuse a plain pip install).
if ! $PY -c 'import pefile, capstone' 2>/dev/null; then
  if [ ! -x .venv/bin/python ] || ! .venv/bin/python -c 'import pefile, capstone' 2>/dev/null; then
    say "  Making a Python environment for the lift (.venv)..."
    if ! python3 -m venv .venv >> "$LOG" 2>&1; then
      rm -rf .venv
      say "  Debian, Ubuntu: sudo apt install python3-pefile python3-capstone"
      say "  Fedora:         sudo dnf install python3-pefile python3-capstone"
      fail "the lift needs Python's pefile and capstone: install them (above), or python3-venv, and run ./setup.sh again."
    fi
    run "Installing pefile and capstone into it" .venv/bin/python -m pip install -q pefile capstone
  fi
  PY=$ROOT/.venv/bin/python
fi
say "  python: $($PY --version 2>&1), cc: $(cc --version | head -1)"

# The toolkit: PCRECOMP, else ../pcrecomp (where the launcher puts it), else ../tools.
if [ -z "${PCRECOMP:-}" ]; then
  for d in ../pcrecomp ../tools; do [ -d "$d/runtime/win32hle" ] && PCRECOMP=$(cd "$d" && pwd) && break; done
fi
[ -n "${PCRECOMP:-}" ] || fail "no pcrecomp toolkit beside this folder: git clone https://github.com/sp00nznet/pcrecomp ../pcrecomp"
[ -f "$PCRECOMP/runtime/win32hle/lz32.c" ] || fail "the pcrecomp at $PCRECOMP is too old for eJay (no win32hle/lz32.c): update it (git -C \"$PCRECOMP\" pull)."
export PCRECOMP
say "  pcrecomp: $PCRECOMP"

# ------------------------------------------------------------------ the title
step 2 "Finding your eJay 2"
# engine DLL -> id | name | palette. Dance eJay 3 (pxd32d5) draws but its
# mixer never arms, so it is not offered yet (ROADMAP).
title_of() {
  case "$(echo "$1" | tr 'A-Z' 'a-z')" in
    pxd32d4.dll) echo "dance2|Dance eJay 2|DANCE2" ;;
    pxd32h4.dll) echo "hiphop2|HipHop eJay 2|hh2" ;;
    pxd32r4.dll) echo "techno|Techno eJay|RAVE2" ;;
    pxd32d5.dll) echo "dance3" ;;
  esac
}
# The program folder under $1: the one with pxd32cl1.dll and an engine beside it.
find_program() {
  find "$1" -maxdepth 4 -iname pxd32cl1.dll 2>/dev/null | while read -r f; do
    d=$(dirname "$f")
    for e in "$d"/*; do t=$(title_of "$(basename "$e")"); [ -n "$t" ] && { echo "$d|$(basename "$e")|$t"; break; }; done
  done | head -1
}
# The sample library under $1: a folder with an .INF whose archive is beside it.
find_library() {
  find "$1" -maxdepth 4 -iname '*.inf' ! -iname autorun.inf 2>/dev/null | while read -r f; do
    [ -f "${f%.*}" ] && grep -q '\[SAMPLES\]' "$f" 2>/dev/null && { dirname "$f"; break; }
  done | head -1
}
MNT=""
cleanup() { [ -n "$MNT" ] && { [ "$MAC" = 1 ] && hdiutil detach -quiet "$MNT"; rm -rf "$MNT" 2>/dev/null; }; }
trap cleanup EXIT
# An .iso (named, or the only one in the folder) is opened read-only: mounted
# on a Mac, unpacked to a temporary folder elsewhere (no root needed).
open_iso() {
  MNT=$(mktemp -d "${TMPDIR:-/tmp}/ejay-iso.XXXXXX")
  if [ "$MAC" = 1 ]; then
    run "Opening $(basename "$1")" hdiutil attach -readonly -nobrowse -mountpoint "$MNT" "$1"
  elif command -v bsdtar >/dev/null; then
    run "Unpacking $(basename "$1") (a minute or two)" bsdtar -xf "$1" -C "$MNT"
  elif command -v 7z >/dev/null; then
    run "Unpacking $(basename "$1") (a minute or two)" 7z x -y "-o$MNT" "$1"
  else
    fail "opening an .iso needs bsdtar or 7z: sudo apt install libarchive-tools (or mount it yourself and pass the folder)."
  fi
}
src=""
if [ -n "$GAME" ]; then
  [ -e "$GAME" ] || fail "nothing at $GAME"
  if [ -f "$GAME" ]; then src=$GAME
  else
    isos=$(find "$GAME" -maxdepth 1 -iname '*.iso' 2>/dev/null)
    if [ -z "$(find_program "$GAME")" ] && [ "$(echo "$isos" | grep -c .)" = 1 ]; then src=$isos; else src=$GAME; fi
  fi
else
  for m in /media/"$USER"/* /run/media/"$USER"/* /media/* /mnt/* /Volumes/*; do
    [ -d "$m" ] && [ -n "$(find_program "$m")" ] && { src=$m; break; }
  done
  [ -n "$src" ] || fail "no eJay 2 disc is mounted: insert it, or ./setup.sh --game-dir <the disc, the installed folder, or the .iso>"
fi
case "$(echo "$src" | tr 'A-Z' 'a-z')" in *.iso) open_iso "$src"; src=$MNT ;; esac
prog=$(find_program "$src")
[ -n "$prog" ] || fail "$src holds no eJay 2 program (no PXD32CL1.DLL with an engine beside it)."
IFS='|' read -r PROGDIR ENGINE ID NAME PAL <<< "$prog"
[ "$ID" = dance3 ] && fail "that is Dance eJay 3, which draws but does not play yet: see ROADMAP.md."
# The library is on the disc; an install leaves it there, so look beside the
# program, then on the disc this came from, then on any mounted disc.
LIB=$(find_library "$(dirname "$PROGDIR")")
[ -n "$LIB" ] || LIB=$(find_library "$src")
if [ -z "$LIB" ]; then
  for m in /media/"$USER"/* /run/media/"$USER"/* /media/* /mnt/* /Volumes/*; do
    [ -d "$m" ] && LIB=$(find_library "$m") && [ -n "$LIB" ] && break
  done
fi
[ -n "$LIB" ] || fail "found $NAME, but not its sample library (a PXD folder with an .INF): insert the disc too, or pass the disc with --game-dir."
say "  $NAME: $PROGDIR"
say "  its sample library: $LIB"

# ------------------------------------------------------------------ copy
step 3 "Copying $NAME into game/$ID (about 500 MB, once)"
GAMEDIR=$ROOT/game/$ID
if done_already "$GAMEDIR/PXD/.copied"; then say "  Already there (skipping)."
else
  mkdir -p "$GAMEDIR"
  rm -rf "$GAMEDIR/program" "$GAMEDIR/PXD"
  run "Copying the program" cp -R "$PROGDIR" "$GAMEDIR/program"
  run "Copying the sample library" cp -R "$LIB" "$GAMEDIR/PXD"
  chmod -R u+w "$GAMEDIR"           # a disc's files arrive read-only
  touch "$GAMEDIR/PXD/.copied"
fi

# ------------------------------------------------------------------ lift
step 4 "Lifting $ENGINE and PXD32CL1.DLL to C (a minute)"
if done_already "src/recomp/$ID/gen_gfx/recomp_dispatch.inc"; then say "  Already done (skipping)."
else run "Lifting" "$PY" tools/lift_ejay2.py "$GAMEDIR/program" "$ID" $([ "$FORCE" = 1 ] && echo --force)
fi

# ------------------------------------------------------------------ build
step 5 "Building build-$ID/ejay2 (a few minutes)"
[ "$FORCE" = 1 ] && rm -rf "build-$ID"
[ -f "build-$ID/build.ninja" ] || run "Configuring" cmake -S host -B "build-$ID" -G Ninja -DEJAY_TITLE="$ID" -DPCRECOMP="$PCRECOMP"
run "Compiling" cmake --build "build-$ID"

# ------------------------------------------------------------------ launcher
step 6 "Making the launcher"
ext=sh; [ "$MAC" = 1 ] && ext=command          # a .command opens from Finder
L="$NAME (recomp).$ext"
cat > "$L" <<EOF
#!/bin/sh
# $NAME, recompiled: the workspace with a demo arrangement, until you close the window.
cd "\$(dirname "\$0")/game/$ID/program" && exec ../../../build-$ID/ejay2 --engine $ENGINE --pal $PAL --lib ../PXD --song --ticks 0 "\$@"
EOF
chmod +x "$L"
say "  $ROOT/$L"
say ""
say "Done. Run \"$L\" to play."
