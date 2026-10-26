# One host, three systems: how the eJay 2 host runs off Windows

`host/ejay2.c` is a front end of ours over an eJay 2 title's own two DLLs:
the audio engine (`PXD32D4.DLL`, `PXD32H4.DLL`, `PXD32R4.DLL`) and the
graphics DLL (`PXD32CL1.DLL`). On Windows they load and run as themselves. On
Linux and macOS nothing can run their 1999 x86 machine code, so they are lifted
to C and run on [pcrecomp](https://github.com/sp00nznet/pcrecomp)'s
`runtime/win32hle`, an implementation of the Win32 API on libc and SDL2. This is
how one source does both.

## The pieces

| | Windows | Linux, macOS |
|---|---|---|
| The DLLs | loaded with `LoadLibraryA`, run as they are | lifted by `tools/lift_ejay2.py`, linked into the host, mapped by `host/posix.c` |
| Win32 | the real one | win32hle (`runtime/win32hle`) |
| The host | 32-bit, `host/build.bat` (MSVC, `/MT`) | 64-bit, `host/CMakeLists.txt` |
| Platform layer | `host/win32.c` | `host/posix.c` |
| Audio | DirectSound | win32hle's DirectSound on SDL audio |
| Window | a Win32 window | win32hle's window manager, shown by its SDL screen |

`host/plat.h` is the seam. It has the handful of things that differ by more
than a macro - make the window, pump messages, guard the audio, list a folder,
find a cache folder - and the conventions below.

## The rule: anything the DLLs see a pointer to lives in guest memory

The DLLs are 32-bit. On a 64-bit host the guest's 4 GB is one block of the
host's memory (win32hle's `guest_mem.c`), and a guest pointer is an offset into
it. So a host pointer handed to the engine - a status word, a file name, the
record `ALoad` fills in - has to point into that block, or the engine reads
something else entirely.

- `gmem(n)` allocates in guest memory (`hle_alloc`; `calloc` on Windows).
  The records the DLLs write (`ALOADREC`), `APlay`'s status words (the engine
  writes them later, from its own thread), the sample library and the paint
  structures live there.
- `GVA(p)` is a host pointer's guest address. Off Windows it stops the run,
  naming the pointer, if `p` is not in guest memory: a silent truncation would
  hand the engine a wild pointer.
- `HOSTP(va)` is the reverse, for reading the engine's own state in place.
- `GSTR(s)` and `gcopy(p, n)` copy something short-lived into a 256 KB ring of
  guest memory. Fine for anything a call reads and lets go of - every file name
  `APlay` is given is copied into the entry by `lstrcpyA` before it returns.
- `X(fn, ...)` calls a DLL export at guest address `fn`, stdcall, with 32-bit
  arguments: a switch on the argument count on Windows, `hle_call_guest` off it.

Off Windows a Win32 call the host makes itself (`BitBlt(...)`) is a macro onto
win32hle's shim of the same name, called the way a guest would call it
(`W(name, ...)` resolves it once per call site). A handle is a 32-bit number on
both sides, so the drawing code is the same source; the few calls that take a
pointer have helpers (`TEXTOUT`, `FILLRECT`, `FONT`, `dib_section`).

## Reading pixels back

The paint check and `--shot` used `GetDIBits`, which win32hle does not have. Both
now blit into a 32-bit top-down DIB section of the host's own and read its
pixels in place (`grab()`), which is the same on both sides. The chrome the
paint check compares against is read straight from the pixels and palette
`ALoad` handed back: on Windows, blitting ALoad's DC into a second one comes out
black.

## The lifted DLLs

`tools/lift_ejay2.py <program folder> <title>`:

1. Copies the engine and the graphics DLL to `work/<title>/`. Both ask for
   `0x10000000`, so the graphics DLL is rebased to `0x10100000` (only its
   `HIGHLOW` relocations change - pefile's `relocate_image` also rewrites the
   import lookup table, which the loader then cannot read; civ3 found that).
2. Catalogs each with `disasm32`, seeded with the vtables `vtable_scan` finds.
   `PXD32CL1.DLL` is MFC 4.2, statically linked, and a method reached only
   through a vtable is called by no `CALL`.
3. Lifts every cataloged function with `lift32` (precise carry), adding any
   branch target outside the catalog until none are new, into
   `src/recomp/<title>/gen_{engine,gfx}/`.

About 20 seconds for both. Nothing it writes is committed: it is derived from
the title's binaries.

`host/posix.c` reserves both address ranges before anything else runs - the
engine's `DllMain` allocates, and its heap would otherwise land where the
graphics DLL has to go - then maps each image where it was lifted, binds its
imports to win32hle (an import win32hle lacks becomes a stub that names itself
when called), and runs its `DllMain`. `GetProcAddress` reads the export
directory in guest memory. The host's window procedure is registered as a
win32hle shim, so win32hle's window manager calls it as it would a guest's.

## What win32hle needed

The engine and an MFC DLL ask for things no game on win32hle had asked for.
These went into pcrecomp as their own changes:

| | Why |
|---|---|
| `mixerOpen` & co., `waveIn*` answering "no such device"; `waveOutSetVolume`, `GetVolume`, `GetID` | `AInit` opens the card's mixer and its recording input before it starts DirectSound |
| `GetProcessVersion`, `GlobalFlags`, `DuplicateHandle`, `LockFile`, `GetSysColorBrush` | MFC's start-up, in `PXD32CL1.DLL`'s `DllMain` |
| `LoadImageA` and `LoadBitmapA` making real DIB sections; `GetObjectA` | the sample blocks are drawn from six texture `.bmp` files that `GFX_SampleAddTexturePair` loads and reads back through `GetObjectA`'s `bmBits`. With the old placeholder handle every block drew black |
| `OpenFile`, `_hread`; LZ32 with SZDD expansion | `ALoad` opens every bitmap through `OpenFile`, and three of the artwork files are SZDD-compressed |
| Small Fonts, Terminal and Fixedsys drawn unsmoothed | the block labels are drawn in Small Fonts and read back; an antialiased stand-in's grey edges came out as stray green and magenta |

## Guarding the speakers

The engine fills its DirectSound buffer only while its mixer is armed. On
Windows an unfilled buffer is whatever was in that memory - full-scale noise -
so the host holds its audio session muted until the engine's armed flag goes
up. win32hle's buffers start zeroed, so off Windows the guard only matters for
volume, but it is the same code. The flag's address is found in the engine's
own code (see [TITLES](TITLES.md)).

## Testing without a screen or speakers

Off Windows, `--headless` runs with no window (win32hle's screen has a headless
mode), and SDL's own drivers do the rest:

```
SDL_AUDIODRIVER=dummy  ejay2 --headless --song --selftest --paintcheck --dragtest --ticks 200
SDL_AUDIODRIVER=disk SDL_DISKAUDIOFILE=out.raw  ejay2 --headless --song --ticks 1000
```

The second writes exactly what the mixer produced, as 44.1 kHz 16-bit stereo,
in real time - which is how the arrangements' timing was checked here: the
loudness envelope of `out.raw` against the bars the blocks were placed at.

## Known differences

- Off Windows `--onset` and the envelope report have no endpoint meter to read
  and stay quiet.
- The block labels' font is Liberation Sans standing in for Small Fonts:
  the same size, slightly different shapes.
- Built and run on Linux x86-64 (Debian 13) and macOS 15 on x86-64. An Apple
  silicon Mac builds the same way (64-bit, guest memory at a fixed address) but
  has not been run.
