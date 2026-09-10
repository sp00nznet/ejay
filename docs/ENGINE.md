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

`APlay` takes 24 bytes, and they are **not** twelve words. Reading every frame
slot the function touches, and letting the `adc`/`mov eax` pairs mark which
slots are halves of a 32-bit value, gives seven arguments:

| Frame slot | Width | What it is |
|---|---|---|
| `bp+0x1A` | dword | **the command** (low word at 0x1A, high at 0x1C) |
| `bp+0x16` | far ptr | a path |
| `bp+0x14` | word | never read |
| `bp+0x12` | word | a record index - scaled by 0x102 (258) |
| `bp+0x0E` | dword | `adc dx, [bp+0x10]` pairs it with 0x10 |
| `bp+0x0A` | dword | `cmp dword ss:[bp+0xA], 0` |
| `bp+0x06` | dword | `mov eax, dword ss:[bp+0x6]` |

4 + 4 + 2 + 2 + 4 + 4 + 4 = 24. The command dispatches four ways:

| Command | What it does |
|---|---|
| 1 | copies the far pointer's string into DGROUP at `0x3686` |
| 2 | copies it into DGROUP at `0x3762` instead, then calls `671A` |
| >= 3 | **starts a voice** |
| other | falls through to `63E8` |

Command 1 and 2 measure the string with `repnz scasb` before copying, so both
are "remember this path" - two different paths, kept in two different places.

### The voice

For command >= 3 the command value *is* the sample index:

```
si   = arg(bp+0x12) * 0x102 + 0x8BC      258-byte records, based at DGROUP 0x8BC
ax   = [si+0x4C] * 0x13E + [si+0x46]     318-byte records
cmd -= 3                                  the command becomes an index
if (cmd >= 0x640) return                  bounds check: 1600 slots
if ([cmd + 0x121E] <= 0) goto 64B6        a 1600-entry byte table
```

So `APlay(3 + slot, ...)` starts slot `slot`, and `bp+0x12` selects one of the
258-byte per-track records. 1600 slots is the sample library; the byte table at
`0x121E` says which of them are usable.

## Loading a .PXD happens on the timer, not on APlay

`APlay` does not read a file. It marks a voice, and the **ATimer tick** does
the loading:

```
ATimer (5CB0) -> 4D88 -> 4D9E -> 4ED4 -> 4EF8 -> 4F80 -> 50CE -> OpenFile
```

Calling `APlay(3, ...)` and then pumping the clock produces, on a later tick:

```
OpenFile(<data dir>\ba\aaaa.pxd) -> FAIL
```

which is the engine building a path *for itself* out of a slot record - a
two-letter directory and a four-letter name, the same shape as the disc's own
`AA\BINP.PXD`. With the slot table still empty the letters all come out as the
first of their range, so it asks for `aaaa.pxd` and gets nothing. Neither
command 1 nor command 2 changes that name: the path is built from the slot
record, not from the string they store.

The parser that consumes the loaded bytes checks `cmp ax, 0x5074` - the two
bytes `74 50`, `tP` - against an in-memory buffer, reached through the
huge-pointer arithmetic that `__AHSHIFT` controls. That constant was garbage
until today, which is one of the places the fix mattered.

## How the library gets indexed

`APlay` command 2 does not remember a *file*. It remembers the sample
library's **root directory**, and the engine concatenates it directly - no
separator inserted - onto a pattern it builds itself. Passing a file path
produces exactly the nonsense you would expect:

```
findfirst <root>AA\BINP.PXDba\????.pxd -> none
```

The right argument is a directory ending in a separator. With that, the
sequence after command 2 is:

1. `671A` zeroes 400 dwords at `0x121E` - the 1600-slot table - and clears the
   counter at `0x11D6`.
2. `673C` builds two-letter directory names: `'b'`, then `'a' + i` for
   `i` in `0..0x16`, giving `ba` through `bw`, and branches at 23 into the
   `a` range. The disc's own directories are `AA`-`AL` and `BA`-`BW`.
3. For each one it searches `<root><dir>\????.pxd` - four-letter names, which is
   why an empty slot record produces `aaaa.pxd`.

### The search goes through INT 21h, not the Win16 API

This is the part that had the whole thing stuck. Borland's C runtime implements
`findfirst`/`findnext` on raw DOS calls, routed through `KERNEL.DOS3Call`, and
a stub that fails every INT 21h fails *silently*: the enumeration finds
nothing, the table stays zero, and every sample name comes out as `aaaa`.

The calls it makes, in order, per directory:

| AH | | |
|---|---|---|
| 30 | get DOS version | once, at startup |
| 2F | get DTA | before each search |
| 1A | set DTA | to a stack buffer, and back afterwards |
| 4E | find first | `<root><dir>\????.pxd` |
| 4F | find next | until it reports no more |

`runtime/win16/dos.c` implements these over `FindFirstFileA`/`FindNextFileA`,
keeping the Win32 search handle in a side table keyed by the DTA address - a
HANDLE does not fit in the 21 reserved bytes DOS used, and the guest never
looks inside them. Names are reported 8.3 and upper-cased, because the engine
parses what it gets back.

With that in place the engine reads its own library off the disc:

```
[dos] findfirst .../ba/????.pxd -> ok
[dos]   -> ABFF.PXD (32829 bytes)
[dos]   -> ABGO.PXD (32955 bytes)
[dos]   -> ABHQ.PXD (33007 bytes)
```

### Indexing is slow, and it is not I/O

About one sample per second, with **no file opened per sample** - the cost is
entirely lifted CPU. The engine exports `ASortInit`, `ASortIn`, `ASortStart`
and `ASortStart2`, so the likeliest explanation is an insertion sort into the
1600-slot table: an O(n^2) shuffle of 258-byte records, which is cheap in 1997
machine code and expensive when every guest instruction is a C statement.
ponytail: measure before optimising - this may just be what the lift costs.

## AWaveDauer reads a .PXD directly

`AWaveDauer(far char *path)` is the one export that opens a sample by name.
It is also the cheapest probe we have on the format:

```
OpenFile(AA\BINP.PXD)  ->  ok
_hread -> 270 of 270        a 270-byte header
returns dx:ax = 604,800
```

Across seven samples of near-identical file size, the answer is almost always
604,800, once exactly double that, and once 774,312 - so it is a decoded or
musical measure, not a file measure. 604,800 / 44,100 = 13.714 s, which is
eight bars at 140 BPM, Dance eJay's own default tempo. That reading fits the
doubling but is inference from the product; the header fields have not been
decoded yet.

## The .PXD header

Magic `tPxD` at offset 0, then a length-prefixed name:

| Offset | | |
|---|---|---|
| 0 | 4 bytes | `74 50 78 44` = `tPxD` |
| 4 | 1 byte | length of the name that follows |
| 5 | that many | the sample's display name, ASCII, may contain CRLF |

Of the 270 header bytes the engine reads, only 15 are identical across three
different samples, so nearly all of it is per-sample data rather than a fixed
structure.

## What is still unknown

- **How a slot gets its name.** The engine builds `<dir>\<name>.pxd` from a
  slot record, and with the table empty every letter comes out as `a`. Whatever
  fills that table - `ACache`, `ASortIn`, or `APlay` command 2's call into
  `671A` - is the last thing between here and a sample actually loading.
- What the three trailing dwords of `APlay` carry. Position, volume and pitch
  is the obvious guess from the product, but it is a guess.
- What `AStart`'s 4-byte argument selects. It clearly wakes the mixer: the
  per-tick work jumps twelvefold.
- The 270-byte header's fields beyond the magic and the name.
