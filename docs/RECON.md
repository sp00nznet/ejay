# Recon: what is actually on the two discs

Everything here was read out of the retail binaries with
[pcrecomp](https://github.com/sp00nznet/pcrecomp)'s `tools/pe/pe_analyze.py`
and `tools/ne/ne_parse.py`. Nothing is guessed and nothing is from a wiki.

## The two discs

| | Dance eJay | Dance eJay 2 |
|---|---|---|
| Disc | `dance-ejay.iso`, 690 MB | `Dance_eJay2.iso`, 675 MB |
| Program dir | `DANCE\DMACHINE\` (runs off the CD) | `D_ejay2\ejay\` (installed from `PXD\DANCE20`) |
| Front end | `DANCE.EXE` — NE, 225 KB | `Dancejay.exe` — PE32, 1.4 MB |
| Front-end language | Visual Basic 4.0, 16-bit | Visual Basic 5.0, 32-bit |
| Audio engine | `DANCE02.DLL` — NE, 36 KB | `PXD32D4.DLL` — PE32, 158 KB |
| Graphics | VBX controls + VB forms | `PXD32CL1.DLL` — PE32, 198 KB |
| File dialogs / DB | in the VB forms | `PXD98DB.DLL` — PE32, 89 KB |
| Sample format | `.PXD` | `.PXD` |
| Song format | (in-form) | `.SCT` |

Both are by **PXD Musicsoft Inc.** The audio DLL carries its author's name in
its own non-resident name table: *"DanceMachine Audio-DLL Copyright B. Throll,
THROLL GmbH, Germany"*.

## The finding that decides the whole project

`DANCE02.DLL` (1997, 16-bit) exports 31 functions. `PXD32D4.DLL` (1999,
32-bit) exports 87. **27 of the 31 are the same names**:

```
ABilder  ABildpos  ADevice   AEnd       AExport   AGetFree  AGetFull
AGetTime AInit     ALautGet  ALautSet   APlay     ARecInit  ARecInput
ARecPlay ARecStart ARecStop  ARecTimer  ASortIn   ASortInit ASortOut
ASortStart ASortStart2 AStart AStop     ATimer    AWaveDauer
```

Only `ACache`, `AHook`, `ATicker` and `DanceTimer` were dropped. The other 60
exports in the 32-bit DLL are additions — a `RWave*` family (recording,
filtering, saving), a `DPlay*` family (file playback), `Fade`, `RDrum`,
`REffekt`.

That is not a coincidence and it is not a reimplementation. **eJay 1 and eJay 2
run the same audio engine, ported from 16-bit to 32-bit and then grown.** The
user's hunch that the first two are close before the spin-offs diverge is
correct, and it is provable at the symbol level.

The German names (`Lautstärke` = volume, `Bild` = picture, `Welle` = wave,
`Pegel` = level, `Fenster` = window, `Mitte` = middle, `Takt` = beat,
`Dauer` = duration, `Pfad` = path, `Zeit` = time) are the original author's,
which means we get most of a symbol table for free.

## Dance eJay: the front end is p-code

`DANCE.EXE` is an NE with 18 segments, 212,876 bytes of "code" — and:

- exactly **one** imported module: `VB40016`
- **216 relocations** in 212 KB
- 17 code segments, every one of them `DISCARDABLE`, all ~15 KB

A native 16-bit Windows program of that size has thousands of relocations and
imports `KERNEL`, `USER` and `GDI` directly. This one calls no Windows API at
all. Visual Basic 4 had no native compiler — 16-bit VB4 emitted **p-code only**
— so those 17 segments are VB p-code streams interpreted by `VB40016.DLL`, not
x86.

**`lift16` cannot do anything with this file.** Feeding it in would produce
200 KB of confident garbage. Its forms are recoverable as structured data
(`Dancemix`, `DanceMachine`, `LoadSave`, `Funktion`, `DMSETUP`, `_IID_MSG`,
`_IID_FILE`, `_IID_MENU`, `_IID_DANCEMIX`, `_IID_DMSETUP` are all in the
string table), and it hosts two VBX controls, `MCI.VBX` and `CMDIALOG.VBX`.

There is also a `GRAFIK\` subfolder shipping `VBRUN300.DLL` and `EJAY.EXE` —
a Visual Basic **3** helper app, one generation older again.

## Dance eJay 2: the front end is native

`Dancejay.exe` is a PE32 whose entry point is the VB5 idiom:

```
00414B6C  push 0x414d78          ; -> "VB5!" project header
00414B71  call ThunRTMain
```

It imports **190** functions from `MSVBVM50.DLL`, including `_CIcos`,
`_adj_fptan` and `_adj_fdiv_m32`. A p-code VB5 binary imports about five
(`ThunRTMain`, `MethCallEngine`, the `EVENT_SINK_*` trio). 190 imports, 1.27 MB
of `.text` and 133 KB of relocations mean this one was **compiled to native
x86**, and `lift32` can lift it.

Its nine `KERNEL32` imports are worth naming, because they are the whole of its
direct OS surface: `GetSystemDirectoryA`, `GetVolumeInformationA`, `OpenProcess`,
`CreateFileA`, `CloseHandle`, `GlobalAlloc/Lock/Unlock/Free`.
`GetVolumeInformationA` plus `OpenProcess` is a CD / already-running check.

Everything else it does, it does through `MSVBVM50` or through the three PXD
DLLs.

## The three 32-bit DLLs

| DLL | Built | Code | What it is |
|---|---|---:|---|
| `PXD32D4.DLL` | 1999-01-26, MSVC 5.0 | 120,700 B | The audio engine. 23 `WINMM` imports (`waveOut*`, `waveIn*`, `timeGetTime`), DIB blitting for the waveform display, `LZ32` for packed reads. 87 exports. |
| `PXD32CL1.DLL` | 1998-10-06, MSVC 5.0 | 138,830 B | Graphics. 115 `USER32` + 36 `GDI32` imports, `CreateDIBSection`/`BitBlt`. 23 exports, all `GFX_*`: the sample grid (`GFX_SampleZeichne` — *zeichne* = draw), the volume bars, the intro sequence. |
| `PXD98DB.DLL` | MSVC 5.0 | 57,628 B | The load/save layer. `GetOpenFileNameA`/`GetSaveFileNameA`, 22 exports: `DoMixLoadDlg`, `DoMixSaveDlg`, `DoWaveImportDlg`, `DoWaveExportDlg`, plus a `VB_CallBack` export — the front end hands it a VB callback. |

`PXD32CL1.DLL` imports `COMCTL32` and `WINSPOOL` and has a large `USER32`
surface for something that only exports 23 drawing calls: it is an MFC-shaped
DLL with a thin exported face.

## What this means for the lift

1. `DANCE02.DLL` is 30,904 bytes of code in **one** code segment and one data
   segment — small model, no far-call graph to reconstruct, 313 relocations,
   34 named entry points. It is the friendliest 16-bit target imaginable and it
   is the direct ancestor of the engine we actually want.
2. `PXD32D4.DLL` is the real prize: the same engine, 32-bit, with 87 names on
   it. Lifting `DANCE02` first buys a second opinion on every shared function.
3. `Dancejay.exe` is liftable but the work is not the lift — it is answering
   190 `__vba*` calls. The `runtime/hybrid/` path (leave `MSVBVM50.DLL` real,
   run the app body lifted) is the same trick that made `encarta` work with
   MFC, and it applies here unchanged.
4. `DANCE.EXE` is not liftable at all. It gets decoded, not recompiled.
