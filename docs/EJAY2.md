# Dance eJay 2: reading a program that hides its own calls

The 1999 application splits into three pieces: `Dancejay.exe` (Visual Basic 5,
native-compiled), `PXD32D4.DLL` (audio) and `PXD32CL1.DLL` (graphics). Both DLLs
are ordinary 32-bit code importing only DLLs Windows still ships, so they load
and run today as they are. The interesting problem was never getting them to
load - it was working out how to call them.

## Dancejay.exe does not import the DLLs it uses

Its import table lists `MSVBVM50.DLL` and `KERNEL32.DLL`, nothing else. It
still calls 143 functions across the two eJay DLLs, plus gdi32 and kernel32
directly, because VB's `Declare Function` is resolved lazily through a thunk the
compiler emits per API:

```
    A1 <cache>          mov  eax, [cache]       ; resolved address, or null
    0B C0 74 02 FF E0   or / jz / jmp eax       ; fast path
    68 <descriptor>     push offset descriptor  ; {dll*, proc*, flags, cache}
    B8 <resolver> FF D0 call resolver
    FF E0               jmp  eax
```

The descriptor holds pointers to the DLL name and the export name, so the thunk
address maps back to a name, and every `call <thunk>` in the program is a call
to that export with its arguments pushed in front of it. `tools/vb_declares.py`
recovers all 217 thunks and disassembles their call sites.

That turns guesswork into reading. Everything below came out of it.

## The start-up order is not the 1997 one

```
ATyp(3) -> ADevice(0) -> AInit(hwnd) -> GFX_AnimationSwapInit(400, 400)
```

`ATyp` first is the part that matters: it selects the engine profile (values
under 100 land in one global, 100-199 and 200-299 in two others), and `ADevice`
probes formats against it. The 1997 order - `AInit` first - leaves the profile
unset.

Two return values also read backwards if you assume booleans:

- **`ADevice(0)` returns a format tier, not a device count.** It calls
  `waveOutGetNumDevs`, walks the devices asking `waveOutGetDevCaps` for a
  particular `dwSupport` bit, then tries `waveOutOpen` with `WAVE_FORMAT_QUERY`
  on 44100Hz/16-bit/stereo. Success returns **0**. The 1997 engine returned a
  count, so a zero here looked like "no devices" when it means "your card takes
  the best format I have".
- **`AInit(hwnd)` returns the engine error word.** 0 is success.

`AInit` takes the window handle - the same global `AFenster` writes. An earlier
pass here read it as taking nothing, from a `ret` that a desynced linear
disassembly found in the middle of a jump table. `tools/dll_signatures.py`
walks the control-flow graph instead and reports an export as unknown rather
than guessing when the walk finds two different purge values.

## ALoad is the whole art pipeline

```c
typedef struct {
    int   hdc;      /* out: memory DC with the DIB selected */
    int   w, h;     /* out: biWidth, biHeight               */
    void *bits;     /* out: DIB section pixels              */
    void *pal;      /* out: the 256-entry colour table      */
    char  name[220];/* in:  filename, a VB fixed string     */
} ALOADREC;
```

`ALoad(&rec)` opens the file, checks for the `SZDD` magic and routes through
`LZOpenFile`/`LZRead` if it finds it (three of the `GRAFIKA` files are
compressed), parses the BMP header, `CreateDIBSection`s it, reads the pixels and
selects it into a fresh `CreateCompatibleDC`. The graphics DLL never sees a
file: it is handed a live DIB. Every `GFX_*Init*` call is
`(rec.hdc, rec.w, rec.h, rec.bits)`, and `GFX_IntroInitScreen` adds `rec.pal`.

## The UI is data, not form code

`SEITEN` ("pages") names every screen and its controls; `K_640`, `K_800`,
`K_1024`, `K_1152` and `K_1280` give ten numbers per control per resolution;
`FONTS` gives the typefaces per resolution; the `GRAFIK*` folders hold the
bitmaps with their extensions stripped. So the workspace layout is a text file,
not compiled VB - which is the good news for rebuilding it.

```
:Hauptbild        EJAY01 EJAY02       the workspace
:Drumpads         :Mixer  :Timestretch  :Effect  :Soundgruppen
:Splashscreen     EJAY30
:Intro            EJAY31 EJAY33 EJAY32
```

## Sound comes out over DirectSound

`PXD32D4` imports `waveOutOpen`, `waveOutSetVolume`, the mixer calls and
`timeGetTime` - but no `waveOutWrite`. It enumerates and sets levels through
winmm and then plays through DirectSound (`ole32.CoCreateInstance`, with an
`ADSoff` export to turn it off again). Two consequences:

- `waveOutSetVolume` does not govern what you hear.
- **`ALautSet` is 0..32768 on an exponential curve, not a percentage.** The
  argument is divided by 32768, fed through `exp()`, scaled and inverted into a
  0..65535 DirectSound attenuation. `ALautSet(10)` is silence, not a tenth -
  a "quiet" bring-up run that passes a percentage straight through proves
  nothing, because nothing was ever audible.

## DPlayFile previews a sample; it does not play a song

Direct file playback, which is how eJay 2 previews a sample, is:

```
AFenster(hwnd) -> DStart(0) -> DPlayFile(path, 0, channel)
```

with `channel` = index + 9 in the original, twelve of them. `DPlayUpdate()` on a
clock, `DPlayFileCheck(channel)` for done, `DGetZeit(channel)` for position -
which is a byte offset into a 44100Hz 16-bit stereo stream and advances at
176,400 a second, i.e. in real time.

Measured on the default endpoint with `IAudioMeterInformation`: peak 0.08 at
`ALautSet(2%)`, 0.20 at 5%, 0.42 at 10%. The level tracks the knob.

**What you give it matters.** `DPlayFile` decodes one sample. `DINTRO.PXD` is a
mix, not a sample, and handing it over gets its bytes rendered as PCM - static,
and the kind that is easy to mistake for a wrong sample rate or a byte-order
bug. `METRO.PXD`, the metronome, plays as a metronome.

The cheap way to tell the two cases apart is to hook `CreateFileA` in the
engine's import table and watch. Samples do not come through `OpenFile` - that
is `ALoad`'s bitmap path - they are memory-mapped: `CreateFileA`,
`CreateFileMappingA`, `MapViewOfFile`. A real preview logs the open; the
sequencer path below logs nothing, because it never gets as far as the file.

## The song path is mapped, and stops in a knowable place

`APlay` places a sample on a track. Its twelve arguments, read off its own code
rather than the call site:

| arg | meaning |
|---:|---|
| 1 | voice, stored as a word at the entry +0x10 |
| 2, 3 | stored at entry +0xFC, +0x100 |
| 4, 5 | stored at entry +0xF4, +0xF8 |
| 6 | pointer to the caller's status word, stored at +0xF0 |
| 7 | the filename, `lstrcpyA`d into the entry at +0x12 |
| 8 | track index - `* 0x84 + 0x100429C8` picks the track record |
| 9 | start position, rounded down to a multiple of 4 |
| 10 | length; entry+4 becomes start+length, or 0x6921CFF0 if zero |
| 11 | stored at entry +8, also rounded to 4 |
| 12 | stored at entry +0x104 |

and it begins by comparing a global against `0x2A90CB20`, returning 0 without
doing anything if it does not match. That is the engine's "memory is up" flag,
set at the end of the allocation pass `AInit` runs, and most of the interesting
exports check it.

Dancejay's own start-up order around it is:

```
ASetPfad(dir) -> AStop -> AMitte(0,0) -> RWaveParam(60, 0x6666) -> ASetFader(0,0)
  -> APlay(0,0,0,0,0, &status, file, 0,0,0,0, 0x100)
```

and its play button then does `ASetFader(0,0)` -> `AStart(0xA17FC0)` and polls
`AGetTime(0)`. 0xA17FC0 is 10,584,000 - four minutes at 44,100 - so `AStart` is
being told how long the arrangement is.

Running exactly that: the ready flag is set, `APlay` places the sample (the
track record's count at +0x6A goes to 1), and `AStart`'s handshake - it writes 2
to a global and spins until the audio thread zeroes it - is answered. So the
engine is up and its thread is alive.

But `AGetTime` returns 0 and the status word never moves off 99. Two things are
worth knowing about why:

- **`ATimer` is a stub in this engine.** The whole function is `mov eax, 1;
  ret`. In 1997 the host pumped the clock; in 1999 the work is all on the audio
  thread, so pumping it does nothing.
- **`AGetTime` has two sources.** If a word at `+0x39DA4` is 1 it returns a
  global directly; otherwise it falls through to the same clock `DGetZeit` uses,
  which is gated on the "playing" word at `+0x433EC` that only `DStart` sets.
  Finding what sets `+0x39DA4` is where the song path continues.

## The workspace: chrome, then blocks

`:Hauptbild` is `EJAY01` (640x480 chrome, sixteen arrangement lanes) and
`EJAY02` (940x520, the sheet every button state is cut from). The chrome goes
up with one `ALoad` and one `BitBlt`; the blocks in the lanes come from the
`GFX_Sample*` family, in this order:

```
GFX_SampleInit(18, 0xa00, 12, textColour, backColour, "Small Fonts", 0)
    -> the grid's own memory DC, and its entry count reset to zero
GFX_SampleAddTexturePair("TEXTURE.BMP", "TEXTURE2.BMP", "DANCE2.PAL", 12, 25, 112)
    -> 1, the index of the style it just appended
GFX_SampleZeichne(style, 0, width, "name", "group", 0, 0)
    -> renders one block into that DC, at `width` pixels
```

Two things are easy to get wrong here.

**The order is backwards from the obvious one.** `GFX_SampleInit` zeroes the
entry count (`this+0x90c`), so registering texture pairs before it throws them
away - and `GFX_SampleZeichne` then fails its `index > count` guard and returns
0 without drawing, which looks exactly like the arguments being wrong.

**`GFX_SampleZeichne` takes no coordinates** because it does not place
anything: it draws one block into the grid's memory DC and the caller blits it
into whichever lane it belongs in. That is why Dancejay's call site passes a
style index, a zero, one number and two strings and nothing that looks like a
position.

`GFX_SampleInit`'s string is a **font face**, not a sample name - it goes
straight into `CreateFontA` with the second argument as the height, and the
three numbers after it are stored as colours.

The lane geometry, measured off `EJAY01A` rather than guessed: the arrangement
field's dark ground runs x 48..596, the orange lane rules sit 18 pixels apart
from y 16 to y 323, and there are sixteen lanes. Twice eJay 1's eight.

## GFX_IntroRefresh takes a timestamp

Not a page number. The DLL compares its argument against 0xd48, 0xfb9, 0x1770,
0x1ac2, 0x1c75, 0x50dc, 0x5d8e and 0x7148 - milliseconds into a 29-second
sequence, with `OffsetRect` and a `rand() & 3` jitter driving the panels. Feed
it real elapsed time, the way Dancejay's form timer does.

It draws by `GetDC`-ing the window handed to `GFX_SetActiveWindow` and blitting
straight into it, so the host must not let Windows erase the client area
(`WM_ERASEBKGND` returns 1, no class background brush).

## Signatures

Argument counts from `tools/dll_signatures.py`, cross-checked against the VB
call sites where one exists.

### PXD32D4.DLL - 87 exports

| export | args | export | args | export | args |
|---|---:|---|---:|---|---:|
| `ABilder` | 7 | `ABildpos` | 1 | `AClose` | 1 |
| `ACloseAll` | 0 | `ADSoff` | 0 | `ADevice` | 1 |
| `AEnd` | 0 | `AExit` | 0 | `AExport` | 2 |
| `AFenster` | 1 | `AGetFree` | 1 | `AGetFull` | 1 |
| `AGetInput` | 0 | `AGetString` | 2 | `AGetTime` | 1 |
| `AInit` | 1 | `ALautGet` | 1 | `ALautSet` | 1 |
| `ALoad` | 1 | `AMemory` | 0 | `AMitte` | 2 |
| `ANummer` | 0 | `APlay` | 12 | `APos` | 1 |
| `ARecDuplex` | 1 | `ARecInit` | 1 | `ARecInput` | 1 |
| `ARecPegel` | 1 | `ARecPlay` | 0 | `ARecStart` | 0 |
| `ARecStop` | 0 | `ARecTest` | 1 | `ARecTimer` | 0 |
| `ASelectMic` | 0 | `ASetCur` | 1 | `ASetFader` | 2 |
| `ASetPfad` | 1 | `ASetPitch` | 1 | `ASortIn` | 1 |
| `ASortInit` | 0 | `ASortOut` | 0 | `ASortStart` | 0 |
| `ASortStart2` | 0 | `AStart` | 1 | `AStop` | 0 |
| `ATest` | 2 | `ATimer` | 0 | `ATyp` | 1 |
| `AVbInfoCall` | 1 | `AWaveDauer` | 1 | `AWelle` | 3 |
| `AWellePos` | 1 | `BWaveDauer` | 3 | `DCloseAll` | 0 |
| `DGetZeit` | 1 | `DPlayFile` | 3 | `DPlayFileCheck` | 1 |
| `DPlayFileClose` | 1 | `DPlayUpdate` | 0 | `DStart` | 1 |
| `Debimem` | 1 | `Extra` | 0 | `ExtraAus` | 0 |
| `Fade` | 0 | `RDrum` | 3 | `REffekt` | 2 |
| `RGetName` | 2 | `RGetParam` | 1 | `RMenu` | 0 |
| `RRecoSave` | 1 | `RTimer` | 0 | `RWavToTemp` | 7 |
| `RWaveFilter` | 1 | `RWaveGetInfo` | 1 | `RWaveGetTime` | 0 |
| `RWaveLaden` | 5 | `RWaveParam` | 2 | `RWavePause1` | 0 |
| `RWavePause2` | 0 | `RWavePlay` | 0 | `RWaveRec` | 1 |
| `RWaveSave` | 0 | `RWaveSetInfo` | 1 | `RWaveStop` | 0 |
| `RWaveTakt` | 0 | `RWaveTransfer` | 1 | `RpDrum` | 3 |

27 of these carry the same names as the 1997 engine. Widths doubled in the port
as expected from 16- to 32-bit - `APlay` takes 12 arguments here against 6 in
`DANCE02.DLL` - while `ABilder` stayed at 7 and `ATimer` and `ASortStart` take
nothing in both.

### PXD32CL1.DLL - 23 exports

| export | args | export | args | export | args |
|---|---:|---|---:|---|---:|
| `GFX_AnimationPhase` | 11 | `GFX_AnimationSwapInit` | 2 | `GFX_IntroClose` | 0 |
| `GFX_IntroDoScrCapture` | 0 | `GFX_IntroInitLeds` | 4 | `GFX_IntroInitScreen` | 5 |
| `GFX_IntroInitScreenCopy` | 4 | `GFX_IntroInitSplash` | 4 | `GFX_IntroInitText` | 4 |
| `GFX_IntroRefresh` | 1 | `GFX_IntroSetKey` | 11 | `GFX_IntroShowSplash` | 0 |
| `GFX_SampleAddTexturePair` | 6 | `GFX_SampleClose` | 0 | `GFX_SampleInit` | 7 |
| `GFX_SampleZeichne` | 7 | `GFX_SampleZeichneSel` | 7 | `GFX_SetActiveWindow` | 1 |
| `GFX_VolBarClose` | 0 | `GFX_VolBarDraw` | 3 | `GFX_VolBarGetCount` | 0 |
| `GFX_VolBarInit` | 4 | `GFX_VolBarRemove` | 1 | | |

`GFX_IntroInitSplash` and `GFX_IntroShowSplash` have no call site in
Dancejay.exe - the splash belongs to `LOADER.EXE`.

## Running it

`host32/ejay2.exe` drives all of the above. It has to be built 32-bit (the DLLs
are), from the eJay folder so the relative bitmap paths resolve:

```
host32/build.bat host32/ejay2.c host32/ejay2.exe
cd <ejay folder>
ejay2.exe --ticks 2000 --volume 3 --dplay --shot out.bmp
```

`--volume` is a percentage and is scaled into `ALautSet`'s 0..32768 before it
gets there. Keep it low.
