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

## The song path: APlay places, AStart runs, RTimer pumps

`APlay` places one sample on one track. Its twelve arguments, read off its own
code rather than the call site:

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

It begins by comparing a global against `0x2A90CB20` and returns 0 without doing
anything if it does not match - the engine's "memory is up" flag, set at the end
of the allocation pass `AInit` runs. Most of the interesting exports check it.

The whole sequence, Dancejay's own:

```
ASetPfad(dir) -> AStop -> AMitte(0,0) -> RWaveParam(60, 0x6666) -> ASetFader(0,0)
  -> APlay(0,0,0,0,0, &status, file, track, bar*88200, 0, 0, 0x100)   per sample
  -> AFenster(hwnd) -> DStart(0) -> AStart(0xA17FC0)
  then RTimer() every frame
```

`AStart`'s 0xA17FC0 is 10,584,000 - four minutes at 44,100 - so positions are in
samples, and a bar at 120 BPM is 88,200 of them. `AGetTime` reports in
milliseconds.

### RTimer is the pump, and ATimer is not

**`ATimer` is a stub in this engine.** The entire function is:

```
100084C7  push ebp / mov ebp, esp / mov eax, 1 / pop ebp / ret
```

In 1997 the host pumped the clock. In 1999 the work moved onto an audio thread
that `AInit` creates, and the host-side tick became a no-op nobody removed.

But the thread does not do everything by itself. Its loop is:

```
WaitForSingleObject(stop, 0)          exit if signalled
if mode == 1 (export)   Sleep(100)
else                    mixer_tick(); if (flag) RTimer(); dplay_update()
if astart_request == 1  ...; astart_request = 0
if astart_request == 2  AStart_work(length); astart_request = 0
```

and `mixer_tick` refuses to do anything while a word at `+0x3AC68` is zero. The
only code that sets it is inside `RTimer`. So `RTimer` on a clock is what arms
the mixer, exactly as Dancejay's play loop does it.

Without it: `AStart` still opens its own gate at `+0x434A8`, `DStart` still sets
`+0x433EC` and starts DirectSound, the placed sample is still loaded from disc -
and the mixer never fills the buffer, so what plays is whatever was in it.
Full-scale noise, and no engine volume setting touches it, because the engine is
not the one producing it. That is worth knowing before wiring a speaker up: the
host here holds its own audio session muted for exactly as long as `+0x3AC68`
is down.

### Which file goes to which call

`DPlayFile` previews one sample and `METRO.PXD` - a plain RIFF WAV at 44.1kHz
16-bit mono, despite the extension - plays through it correctly. `DINTRO.PXD` is
`tPxD`, a mix rather than a sample, and comes out as static.

The `tPxD` decode lives on the `APlay` path, and it is version-agnostic: the
1999 engine opens, decodes and plays eJay 1's 1996 samples without special
pleading. Which matters, because eJay 2's own library ships on a second disc.

A `tPxD` header is the magic, then a NUL-terminated name written as two lines:

```
74 50 78 44  "Snare Beat
Risk" 00 ...
```

- exactly the pair of strings `GFX_SampleZeichne` takes for a block label, and
what fills the browser. Some names begin with the separator and most carry
stray control bytes, so they need trimming before they render.

## The library index, and a browser that works

eJay does not walk its own disc looking for samples. It ships an index, and
decoding it is the difference between a browser with names in it and a browser
that does something.

```
DMACHINE\PXD.TXT   18 quoted numbers, then four quoted fields per sample:
                   size, length in bars, name line 1, name line 2
DMACHINE\MAX.TXT   one quoted path per sample, same order: "ba\aaaf.pxd"
                   (MIN.TXT is the same list for a minimal install)
```

The 18 leading numbers are nine `(start, count)` pairs, and they add up to
exactly the 1,352 records that follow:

| group | start | count | what is in it |
|---:|---:|---:|---|
| 0 | 0 | 126 | `Grp. 1 / Vers1` ... |
| 1 | 126 | 114 | `Sharp *` |
| 2 | 240 | 115 | `Myth * L / Cut 1` |
| 3 | 355 | 100 | |
| 4 | 455 | 81 | |
| 5 | 536 | 300 | `Clap / 01 L` - percussion |
| 6 | 836 | 229 | |
| 7 | 1065 | 127 | |
| 8 | 1192 | 160 | `Here I am / 01` - vocals |

Nine sound groups, which is what eJay 1's tooltip calls them and what the
category buttons switch between. eJay 2's chrome has twelve (`B_GRUPPE_01`..`12`
- Loop, Drum, Bass, Guitar, Seq, Layer down the left, Rap, Voice, Effect, Xtra,
GrooveG, Wave down the right), so nine of them do something against this library
and three stay dark.

The two name fields are the pair `GFX_SampleZeichne` wants for a block label,
and the bar count is the block's width. So the index gives the browser its rows
and the grid its geometry from the same record.

Geometry measured off `EJAY01A`: the panel at x 176..539, y 366..472; the button
lozenges 49 wide at x 112 and x 569, 16 tall, on a 22-pixel pitch from y 346;
the scroll strip at x 543.

## K_640: eJay's control layout, in a 1280x960 space

`K_640` and its siblings are the coordinate table. Each entry is a control name
followed by ten numbers:

```
name   dx dy   s1x s1y   s2x s2y   s3x s3y   w h
```

`dx,dy` is where the control sits and `w,h` how big it is; the three source
pairs are its states cut out of `EJAY02` - normal, rolled over, pressed - which
is what SEITEN means by `RollOverButton`. A two-state control leaves the third
pair at 0,0 and reuses the second for "on", which is how a selected category
button lights up.

**Every number is in a 1280x960 space, so the 640x480 art set halves them.**
That is the thing to know, and it is not obvious: `B_EJAY` is listed at x 1216
with a width of 63, which reads as nonsense on a 640-pixel screen until you
halve both and get a 31-pixel button at x 608, hard against the right rail where
it belongs. The proof is in the sheet - crop `EJAY02A` at the halved source
coordinates and three states of the eJay logo button come out; crop at the
unhalved ones and you get bits of three unrelated icons.

`K_640`, `K_800` and `K_1280` being near-identical files is the other tell: the
numbers do not depend on the resolution, only the rounding does.

591 controls are listed, covering every page. Checking each one's normal state
against the chrome pixel-for-pixel puts 89 of the `B_*` entries on `:Hauptbild`
and the rest on the other pages, which is a cheap way to tell which page a
control belongs to without parsing SEITEN.

The browser's own geometry comes from the same table rather than from a ruler:
`G_SAMPLE_WINDOW` is x 178..537, y 380..473 once halved, and `K_SAMPLE_VSCROLL`
the strip beside it. Measuring it by eye put the list 14 pixels too high, over
the transport bar.

## The intro stall: one call that should not have been the first

`GFX_IntroRefresh(3000)` returns in 0 ms. `GFX_IntroRefresh(3500)` took
**92 seconds**. So it was never a hang, and it was one call in one band - the
one past 0xd48.

It was not drawing either, which is the part that redirected the search:
counting the graphics DLL's own GDI calls across that one Refresh gives **24
BitBlts and 1 PatBlt in 83 seconds**. Twenty-five GDI calls cannot take a minute
and a half. What eats the time is a software pixel loop inside the DLL -
`movsd`, byte shifts, `ror` - blending two pixels an iteration, and its
iteration count comes out of the element's own fields.

Those fields were zero: the twenty-four VU elements at `intro+0x7990` (0x60
apart) had a zero rectangle at +0x34 and a zero float scale at +0x44. Nothing
clamped the loop.

**The fix is `GFX_IntroRefresh(-1)` as the first call.** Reading the dispatch at
the top of the function:

```
mov  edi, [ebp+8]
cmp  edi, -1
je   first_time            ; -1 goes here
mov  al, [esi+0x368]       ; "started" byte
test al, al
je   animate               ; not -1 and never started -> straight into the body
...
first_time:
  [esi+0x368] = 1; [esi+0x8418] = 1; [esi+0x8419] = 0; [esi+0x80] = 0
  return 1
```

With the started byte clear, **-1 is the only argument that reaches the branch
which sets it.** Any other value walks straight into the animation with the
elements never prepared. Call it once first and the whole 29-second timeline
runs without a stall - every Refresh returns 1, start to finish.

What it does not yet do is show anything: after the proper init the per-frame
path is a different routine, it draws (13 BitBlts and 447 PatBlts over 400
frames, so it is working), and the result stays black. The intro expects to be
fed its progress and level values by the application, and nothing here feeds
them. `--no-introinit` skips the init and gets the old behaviour - the system
check screen painted once, then the stall - which is where the screenshot above
came from.

## What feeds the intro: nothing, and that is the point

There is no value setter in the graphics DLL. The whole intro surface is
`GFX_IntroDoScrCapture`, `GFX_IntroInitScreen`, `GFX_IntroInitScreenCopy`,
`GFX_IntroInitLeds`, `GFX_IntroInitText`, `GFX_IntroInitSplash`,
`GFX_IntroShowSplash`, `GFX_IntroSetKey`, `GFX_IntroRefresh` and
`GFX_IntroClose`. No `SetProgress`, no `SetLevel`, no per-element value at all -
and `Dancejay.exe` has exactly one `GFX_IntroSetKey` call site, inside the loop
that walks the coordinate table.

So the elements animate themselves off the timestamp `GFX_IntroRefresh` is
given. The progress bars fill because time passed, not because anybody told
them a percentage. That is why fixing the first call fixed the timeline: it was
never waiting to be fed, it was walking into an animation it had not been told
to start.

`GFX_AnimationPhase` is not it either - its eleven arguments come off a
0x1CC-stride record array in the application, and its two call sites are in the
workspace, not the intro.

What is still wrong is separate: after the correct init the per-frame path draws
(13 BitBlts and 447 PatBlts over 400 frames) and the result stays black. The
next suspect is `GFX_IntroInitScreenCopy`. Dancejay calls it two ways - with the
screen's own DC and a null pixel pointer when the display is deeper than 8bpp,
and otherwise with a *second* bitmap loaded into its own record at `Me+0x5E8`.
This host only ever does the first.

## GFX_IntroSetKey stores a rect per name

Eleven arguments: a name and ten numbers. The first four are **x, y, w, h** -
the DLL stores `left = a1, top = a2, right = a1 + a3, bottom = a2 + a4`. In a
`K_640` record the size is the *last* pair, not the second, so handing it the
ten numbers in file order gives it a source coordinate as a width.

The rect lands at `intro+0x8290 + index*0x10`, and the index comes from the
name: `K_INTRO_VU01` writes at +0x8290, `K_INTRO_VU02` at +0x82A0. The DLL
carries all 57 `K_INTRO_*` strings in its own data to do that lookup; `K_640`
lists 58, the extra being `K_INTRO_REGLER`.

None of that was readable from the disassembly with any confidence. What settled
it was diffing the DLL's whole image across a single call with ten values that
could not occur naturally - every dword that moves is somewhere SetKey writes,
whatever arithmetic it did on the way.

## GFX_IntroRefresh takes a timestamp## GFX_IntroRefresh takes a timestamp

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
