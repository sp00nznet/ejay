# Changelog

All notable changes to this project are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0] - 2026-10-26

The first release: the eJay 2 family on Windows, Linux and macOS, from a
one-click installer or one setup script, with your own disc.

### Added

- **Linux and macOS.** The title's two DLLs are lifted to C
  (`tools/lift_ejay2.py`) and run on pcrecomp's `runtime/win32hle`; the same
  host source builds against the real DLLs on Windows and against win32hle
  elsewhere, as a 64-bit program (`host/plat.h`, `host/posix.c`,
  `host/CMakeLists.txt`; [PORTING](docs/PORTING.md)). Needs pcrecomp at
  7bbc0c0 or later (pcrecomp#135, #140-#143).
- **Techno eJay** (Rave eJay 2 in France, the same program) runs on the host,
  and **Dance eJay 3** draws on it, silently ([TITLES](docs/TITLES.md)).
- **Each title's own sample library.** `--lib` reads an `.INF` and its archive
  (Dance eJay 2's `DANCE20` and kits, `HipHop20`, `RAVE20`, `dance30`) as well
  as Dance eJay 1's `PXD.TXT` index; the `.INF`'s group is the browser button,
  and an archive member is copied out once to a cache folder for the engine.
- **One-step setup.** `setup.sh` (Linux, macOS) and `Setup.cmd` (Windows) take
  a disc, an `.iso` or an installed program folder, pick the title by its
  engine, copy the program and library into `game/<title>/`, lift, build and
  leave a `<Title> (recomp)` launcher.
- **The installer.** `launcher/launcher.json` for sp00nznet/recomp-launcher, and
  `.github/workflows/installer.yml`, which builds the Windows, Linux and macOS
  installers and attaches them to a tagged release.
- `--headless` and `--trace` off Windows, and `--sample N` (one library sample
  alone at bar 0).
- `conform.py` checks the eJay 2 libraries (every `.INF` offset, the members
  back to back, the bar field, one category per group) and finds each disc's
  tempo. Against seven discs: 120,645 passed, 158 failed.
- Docs: [PORTING](docs/PORTING.md), [TITLES](docs/TITLES.md).
- Dance eJay 2 host: drives `PXD32D4.DLL` and `PXD32CL1.DLL` directly in the
  order `Dancejay.exe` uses, into a window of its own.
### Added

- The arrangement page: chrome, sixteen lanes, sample blocks with the samples'
  own two-line names, hover and pressed states from `K_640`, block dragging.
- A working sample browser over `DMACHINE\PXD.TXT` and `MAX.TXT` - category
  buttons, wheel and scroll strip, selection, and double-click placement.
- The start-up system check, through the `GFX_Intro*` family.
- `tools/vb_declares.py`, which recovers the 217 VB5 lazy-resolution thunks in
  `Dancejay.exe` and disassembles their call sites. The start-up order, `ALoad`'s
  record layout and `ALautSet`'s scale all came out of it.
- `tools/dll_signatures.py`: argument counts by CFG walk rather than linear sweep.
- `tools/conform.py`: conformance harness. Parses every sample, control table,
  page list and saved arrangement on a local disc and reports a pass/fail count.
- **A second title.** `--engine`, `--gfxdll` and `--pal` point the host at a
  sibling: HipHop eJay 2 (2000) draws and plays through the same code. Its
  graphics DLL is the same 23 exports, its audio engine a strict superset of
  Dance eJay 2's 87. The host no longer assumes Dance's palette names, and skips
  the system check when a title ships no intro bitmaps, which HipHop does not.
- Host measurement flags: `--onset` (where the playhead is when a beat leaves the
  sound card), `--tracks`, `--paintcheck`, `--file`, `--len`, `--selftest`.

### Fixed

- **A dropped block destroyed the workspace.** Moving a block called
  `ACloseAll`, which frees every bitmap `ALoad` made - the chrome and the button
  sheet - rather than the arrangement. A move now empties the tracks in place
  and places every slot again, earliest first.
- **HipHop eJay 2's tempo.** A bar was Dance eJay 2's 302,400 for every title;
  HipHop eJay 2 is 90 BPM, 470,400. The bar now comes from the library.
- The engine's internal addresses (track records, mixer-armed flag) were
  `PXD32D4.DLL`'s; they are found in whichever engine is loaded.
- Category buttons, the paint check's allowed regions and the controls that
  answer the mouse come from each title's `K_640` and `SEITEN`, not Dance eJay
  2's measurements; the drum pads' `B_PAD_04`, from another page, sat over the
  grid.
- `host/build.bat` finds Visual Studio with `vswhere` instead of one machine's
  paths, and links the C runtime in (`/MT`).
- **Arrangement timing.** A bar was 88,200 - 120 BPM counted in 44,100Hz samples,
  wrong in both tempo and unit. The engine counts output bytes (176,400 a second)
  and Dance eJay is a fixed 140 BPM, so a bar is 302,400. At the old value sixteen
  bars of grid were eight seconds and every block's end fell inside its own audio.
- Sample length is read from the `tPxD` header rather than `PXD.TXT`'s bar field.
- **Track grid geometry.** The grid was eyeballed at y 16..323; `G_SPUREN_SAMPLE_640`
  puts it at 18..298, so the bottom lanes were drawing over the horizontal scroll
  bar. Blocks and the playhead now share one bar-to-pixel mapping, which removes a
  drift of one pixel per bar between the line and the blocks it crosses.
- Browser panel geometry read from `K_640` instead of eye-measured constants,
  which had the first row of the sample list painting on the frame.
- `GFX_IntroSetKey` before the bitmaps, and a real 16-bit copy surface, which is
  what the intro needed - not a value fed to it.

### Changed

- `host32/` is `host/`, and the Windows build lands in `build-windows/`.
- The README is the front door again; the research narrative lives in `docs/`.
- Repository brought in line with the standing repo conventions: changelog,
  roadmap, contributing guide, CI, and a conformance harness with a baseline.

### Removed

- The bring-up probes - import patching, memory snapshots, the Refresh timing
  experiments, `host32/probe.c` - and the unused `runtime2/`. What they found
  is in [EJAY2](docs/EJAY2.md); the code is in the history.

[Unreleased]: https://github.com/sp00nznet/ejay/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/sp00nznet/ejay/releases/tag/v0.1.0
