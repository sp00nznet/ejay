# The 1997 engine, as it behaves when you run it

Everything here was observed by calling `DANCE02.DLL`'s exports in recompiled
code and watching what it asked the operating system for. Where something is
inferred rather than observed, it says so.

## The exports, and what they cost to call

A Win16 export is PASCAL: the callee pops its own arguments, so the argument
size is not a guess, it is the `RETF N` the compiler emitted. IDA reports it
per function and `tools/test_engine.py` asserts the harness agrees.

| Export | Args | Export | Args | Export | Args |
|---|---:|---|---:|---|---:|
| DanceTimer | 16 | AInit | 2 | ATimer | 0 |
| AStart | 4 | AStop | 0 | AEnd | 0 |
| APlay | 24 | AGetTime | 0 | ADevice | 4 |
| ALautSet | 4 | ALautGet | 4 | ATicker | 8 |
| ABilder | 28 | ABildpos | 4 | AGetFree | 4 |
| AExport | 8 | AWaveDauer | 4 | ARecStart | 0 |
| ARecStop | 0 | ARecInit | 4 | ARecTimer | 0 |
| ARecPlay | 0 | ARecInput | 4 | AHook | 8 |
| ACache | 4 | ASortInit | 0 | ASortIn | 4 |
| ASortOut | 0 | ASortStart | 0 | ASortStart2 | 0 |
| AGetFull | 4 | InterStart | 4 | InterEnde | 4 |

## What AInit does

`AInit` is not a device open. It is a memory plan:

```
GlobalAlloc(GMEM_MOVEABLE|ZEROINIT, 1,587,712)     the sample cache
GlobalAlloc(GMEM_MOVEABLE|ZEROINIT, 1,179,848)     a second cache
GlobalAlloc(ZEROINIT, 2,000)
GlobalAlloc(ZEROINIT, 33,024)  x 8                 the mixing voices
timeSetEvent(32 ms) -> seg1:2F4A                   = DanceTimer
```

Eight 33 KB buffers is the shape of the whole product: eight tracks, each with
its own mixing buffer, which is exactly what Dance eJay puts on screen.

It then checks `ds:[0xAC]` against `0x2A90CB20` - a signature the engine writes
during its own startup - and, if it matches, calls `InterStart`.

`AInit` returns `ds:[0xA4]`, and that is **not** a success code: the byte is
only ever touched with `or 0x1`, `and 0xFE`, `or 0x2`, `or 0x8`, `or 0x10` and
so on. It is a status bitmask, and zero means idle. A run that returns 0 has
not failed - which is worth knowing, because it looks exactly like failure.

## There are two clocks, and both have to run

This is the part that is not guessable from the outside.

- **DanceTimer** (`seg1:2F4A`) is what `timeSetEvent` registers, at 32 ms.
- **ATimer** is what the *host* was expected to call on a timer of its own.
  In 1997 that host was `DANCE.EXE`, and its VB form carries an `MCI.VBX` and
  a Timer control.

The waveOut path hangs off **ATimer**, not DanceTimer:

```
ATimer (5CB0) -> 5CEA -> 5CFA -> 5D24 -> 5D72 -> 3660 and 37B4
                                                  |
                    waveOutOpen / waveOutPrepareHeader / waveOutWrite
```

So an engine driven only by its own multimedia timer keeps perfect time and
never queues a buffer. `runtime/main.c --pump` runs both.

The effect is visible in the call count per tick:

| State | lifted calls per tick |
|---|---:|
| after AInit only | 4 |
| after AStart | 48 |
| after AStart, both clocks pumped | ~130 |

## The other four waveOutOpen sites

Four of the six `waveOutOpen` call sites sit at `7399`, `7409`, `7478` and
`74F6`, all inside `ADevice` (`7264`). `ADevice(0)` returns the number of
wave-out devices on the machine, so those are the probe: it opens candidate
devices to find out what they support.

## APlay is a multiplexed command

`APlay` takes 24 bytes, which given PASCAL push order (left to right, first
argument deepest) lays out as eleven arguments:

| Frame slot | Width | What it is |
|---|---|---|
| `bp+0x1C` | word | argument 1 - read out first and stashed |
| `bp+0x1A` | word | argument 2 - **the command**, compared against 1 and 3 |
| `bp+0x16` | dword | argument 3 - a far pointer |
| `bp+0x14` … `bp+0x06` | 8 words | arguments 4-11, not yet identified |

With the command word set to 1, the code takes the far pointer, measures it
with `repnz scasb` and copies it into DGROUP at `0x3686` - so argument 3 is a
string, and command 1 stores a path. Command values `>= 3` and `!= 1` branch
elsewhere and have not been traced.

Calling it with command 1 and a real `.PXD` path is accepted and changes no
observable state, which fits "remember this path" rather than "load this file".
Nothing has opened a `.PXD` yet.

## What is still unknown

- Which `APlay` command actually triggers playback, and what the eight trailing
  words carry - most likely voice, position, volume, pan and pitch, but that is
  inference from the product, not from the code.
- What `AStart`'s 4-byte argument selects. It clearly wakes the mixer: the
  per-tick work jumps twelvefold.
- Where the `.PXD` read happens. `OpenFile` and `_hread` are implemented and
  have never been called, so no path reaches them yet.
