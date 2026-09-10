# Dance eJay 1 + 2 Static Recompilation

Static recompilation of **Dance eJay** (PXD Musicsoft / Fast Trak, 1997) and
**Dance eJay 2** (1998–99) — the loop-and-drop dance studio that taught a
generation of teenagers what a bar was — from their shipping binaries to
native C.

Both are Visual Basic front ends bolted onto a small, fast, hand-written
audio engine. The engine is the part worth having, and it is the part that
survived the version jump intact.

Built on the [pcrecomp](https://github.com/sp00nznet/pcrecomp) toolchain,
alongside the same-era Win16/Win32 arc of DinoPark Tycoon (1993), El-Fish
(1993), Catz (1996) and Operation Neptune (1998).

## Status

**Recon complete. Nothing is lifted yet.** Both discs are torn down, every
binary is identified, and the plan below is built on measurements rather than
guesses. See [RECON](docs/RECON.md) for the full teardown.

## The finding

`DANCE02.DLL` (1997, 16-bit NE) exports 31 functions. `PXD32D4.DLL` (1999,
32-bit PE) exports 87. **27 of the 31 are the same names.**

```
ABilder  ABildpos  ADevice  AEnd     AExport   AGetFree  AGetFull  AGetTime
AInit    ALautGet  ALautSet APlay    ARecInit  ARecInput ARecPlay  ARecStart
ARecStop ARecTimer ASortIn  ASortInit ASortOut ASortStart ASortStart2
AStart   AStop     ATimer   AWaveDauer
```

eJay 1 and eJay 2 are not two engines that resemble each other. They are one
engine, ported 16-bit to 32-bit and then grown by sixty functions. That is the
whole reason to do these two together: **every shared function gets lifted
twice, from two independent compilers, and the two results check each other.**

The names are German — *Laut* volume, *Bild* picture, *Welle* wave, *Pegel*
level, *Dauer* duration, *Fenster* window, *Takt* beat — because the DLL says
who wrote it, right there in its own name table: *"DanceMachine Audio-DLL
Copyright B. Throll, THROLL GmbH, Germany."* We get most of a symbol table for
free.

## What we are actually working with

| | Dance eJay (1997) | Dance eJay 2 (1998–99) |
|---|---|---|
| Front end | `DANCE.EXE`, VB4 **16-bit p-code** | `Dancejay.exe`, VB5 **native x86** |
| Audio | `DANCE02.DLL`, 30,904 B of 16-bit code | `PXD32D4.DLL`, 120,700 B of 32-bit code |
| Graphics | VB forms + `MCI.VBX`, `CMDIALOG.VBX` | `PXD32CL1.DLL`, 23 `GFX_*` exports |
| Load/save | in the forms | `PXD98DB.DLL`, 22 exports |
| Liftable? | DLL yes, EXE **no** | all of it |

The asymmetry matters. `DANCE.EXE` imports exactly one module — `VB40016` — and
carries 216 relocations across 212 KB. It calls no Windows API. VB4 for 16-bit
had no native compiler, so those seventeen discardable segments are p-code, and
`lift16` would turn them into confident garbage. `Dancejay.exe` is the opposite
case: a genuine VB5 native build, 1.27 MB of real x86, 190 `__vba*` imports —
liftable, with the whole VB runtime to answer.

## Roadmap

Each phase ends with something that runs, not something that compiles.

**Phase 1 — `DANCE02.DLL`, the 1997 engine.**
One code segment, one data segment, small model, 313 relocations, 34 named
entry points. `ne_decode` → `lift16`. The harness is a C host that opens a
`.PXD`, calls `AInit`, `ADevice`, `AStart`, `APlay` through lifted code, and
routes `MMSYSTEM` to real `waveOut`. Done when a sample from the 1997 disc is
audible out of recompiled 1997 machine code.

**Phase 2 — the `.PXD` format.**
Written down from the lifted decoder rather than from a hex editor. eJay 1's
samples and eJay 2's `DJ2DG*.PXD` cross-check the result — the same loader
should eat both, and any place it does not is where the format changed.

**Phase 3 — `PXD32D4.DLL`, the 1999 engine.**
`disasm32` → `lift32`, 87 exports pre-named. The same harness, 32-bit. Every
one of the 27 shared functions is now lifted twice from two compilers; where
the two disagree, one of them is wrong, and that is the cheapest bug-finder
this project will ever get.

**Phase 4 — `PXD32CL1.DLL` and `PXD98DB.DLL`.**
The sample grid, the volume bars, the intro; then the load/save layer and the
`.SCT` song format. `GFX_*` draws through `CreateDIBSection` + `BitBlt`, which
is the same shim shape Operation Neptune needed for WinG.

**Phase 5 — `Dancejay.exe`.**
`lift32` the whole 1.27 MB, and leave `MSVBVM50.DLL` **real** — the hybrid
boundary from [pcrecomp's HYBRID](https://github.com/sp00nznet/pcrecomp/blob/main/docs/HYBRID.md),
the same trick that put Encarta's app body in lifted code while MFC stayed
native. Answering 190 `__vba*` calls by hand is the alternative, and it is not
a better one. Also where `GetVolumeInformationA` gets told the CD is present.

**Phase 6 — the eJay 1 front end.**
Not a lift. Decode the VB4 p-code and the form definitions into structured
data, and drive the Phase 1 engine from a rebuilt UI. eJay 1's forms are named
in its string table already: `Dancemix`, `DanceMachine`, `LoadSave`,
`Funktion`, `DMSETUP`.

Phases 1–3 are the spine. If this project only ever ships those, it has still
recovered the engine.

## Layout

```
original/       your copies of the two discs (gitignored — see Legal)
  ejay1/          Dance eJay
  ejay2/          Dance eJay 2
docs/           RECON, and the format notes as they land
tools/          project-specific scripts; the toolchain lives in ../tools
src/engine/     the runtime: CPU model, import bridges, waveOut shim
src/recomp/gen/ generated C (regenerated, not committed)
scripts/        build.ps1
extracted/      extracted assets (regenerated, not committed)
work/           scratch analysis output
```

`../tools` is a checkout of [pcrecomp](https://github.com/sp00nznet/pcrecomp),
the same sibling layout the other pcrecomp-family projects use.

## Reproducing the recon

Python 3.10+ with `capstone` and `pefile`, and your own copies of the discs
in `original/`:

```bash
# The 1997 engine: segments, relocations, 34 named entry points
python ../tools/tools/ne/ne_parse.py original/ejay1/DANCE/DMACHINE/DANCE02.DLL

# The 1997 front end: one imported module, and why that settles it
python ../tools/tools/ne/ne_parse.py original/ejay1/DANCE/DMACHINE/DANCE.EXE

# The 1999 engine, and the two DLLs beside it
python ../tools/tools/pe/pe_analyze.py original/ejay2/D_ejay2/ejay/PXD32D4.DLL
python ../tools/tools/pe/pe_analyze.py original/ejay2/D_ejay2/ejay/PXD32CL1.DLL
python ../tools/tools/pe/pe_analyze.py original/ejay2/D_ejay2/ejay/PXD98DB.DLL

# The VB5 front end
python ../tools/tools/pe/pe_analyze.py original/ejay2/D_ejay2/ejay/Dancejay.exe
```

Nothing builds yet. This section grows as the phases land.

## Legal

No game files are included. Dance eJay is © 1997 and Dance eJay 2 © 1998–1999
PXD Musicsoft Inc. / Fast Trak / eJay AG; the audio DLLs are © Bernhard Throll
/ THROLL GmbH. All long out of print, all still in copyright as far as anyone
can tell. Bring your own discs.

The code in this repository is MIT licensed. See [LICENSE](LICENSE).
