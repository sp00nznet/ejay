# Changelog

All notable changes to this project are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Nothing is released yet: the recompiled eJay 1 engine runs and the eJay 2 host
drives the shipping DLLs, but neither is a build anyone else can pick up and
use without their own discs. `Unreleased` is maintained as work lands.

## [Unreleased]

### Added

- Dance eJay 2 host (`host32/ejay2.c`): drives `PXD32D4.DLL` and `PXD32CL1.DLL`
  directly in the order `Dancejay.exe` uses, into a window of its own.
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

- Repository brought in line with the standing repo conventions: changelog,
  roadmap, contributing guide, CI, and a conformance harness with a baseline.
