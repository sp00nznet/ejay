# The eJay 2 family: which titles run, and what differs between them

The engine shipped under a run of eJay covers from 1998 to 2001. A title is in
the family when its program folder has an audio engine named `PXD32?4.DLL`
(or the later `?5`) and the graphics DLL `PXD32CL1.DLL`. That pair is what
`host/ejay2.c` drives, so a title in the family runs on it with its own
artwork, layout, palette and sample library - the host reads all of those from
the title's own files.

## Where each title stands

| Title | Engine | Tempo | Library | Status |
|---|---|---|---|---|
| Dance eJay 2 (1999) | `PXD32D4.DLL`, 87 exports | 140 BPM | `PXD\DANCE20` + three kits, 3,290 samples | plays, Windows, Linux, macOS |
| HipHop eJay 2 (2000) | `pxd32h4.dll`, D4's 87 + 7 | **90 BPM** | `pxd\HipHop20`, 3,504 in the browser's groups | plays, Windows, Linux, macOS |
| Techno eJay (2000) = Rave eJay 2 (France) | `PXD32R4.DLL`, D4's 87 exactly | 140 BPM | `PXD\RAVE20`, 4,015 samples | plays, Windows, Linux, macOS |
| Dance eJay 3 (2000) | `pxd32d5.dll`, D4's 87 + 9 | 140 BPM | `pxd\dance30`, 4,547 samples | draws, Windows and Linux; silent - its mixer never runs |

Techno eJay and Rave eJay 2 are the same program under two names: their engine,
`K_640`, `SEITEN` and library are byte for byte the same, and only the front
end's file name differs (`TECHNO.EXE`, `R_EJAY2.EXE`). `PXD32CL1.DLL` is
byte-identical in Dance eJay 2 and Techno eJay, and a different build with the
same 23 exports in HipHop eJay 2 and Dance eJay 3.

**Not in the family.** Dance eJay 1 (1997) is a 16-bit `DANCE02.DLL` and a VB4
p-code front end - its own story, in [ENGINE](ENGINE.md). Rave eJay 1 (1998)
and the Dance eJay Superpack (2001, a 32-bit reissue of Dance eJay 1) use
`PXD32RA.DLL` / `PXD32OA.DLL`: 32-bit, but a subset of D4 with 23 of its exports
missing (`ALoad`, `APos`, the `D*` preview family among them) and no
`PXD32CL1.DLL`. House eJay and later keep their pictures in another format.

## What the host reads from each title

None of this is configured per title in the host; each comes from the title's
own files or code.

- **The bitmaps**, `GRAFIKA\EJAY01A` and `EJAY02A`, through the engine's own
  `ALoad`. The system check (`EJAY31A/32A/33A`) is Dance eJay 2's alone; the
  host skips it when the bitmaps are not there.
- **The layout**, `K_640`: every control's rectangle and its three states in
  `EJAY02A`, in a 1280x960 space. The track grid is `G_SPUREN_SAMPLE_640`, the
  browser `G_SAMPLE_WINDOW`, and the category buttons `B_GRUPPE_01`... -
  button *n* for sound group *n*. Dance eJay 2 has six a side; Techno eJay five,
  with 11 and 12 zero-sized; HipHop eJay 2 puts 12 above 10 and 11; Dance eJay 3
  moves them all to the right.
- **The page**, `SEITEN` (`seiten` in HipHop and Dance 3): which controls are on
  which page. `K_640` holds every page's controls and the drum pads
  (`:Drumpads`, `B_PAD_01`...) sit right over the track grid, so only the
  workspace's (`:Hauptbild`) answer the mouse.
- **The palettes** for the three block textures: `DANCE2.PAL`, `DANCE2A.PAL`,
  `DANCE2B.PAL`; `hh2*`; `DANCE3*`. Techno eJay ships `RAVE2.PAL` alone, which is
  then used for all three.
- **The library and its tempo**, from the `.INF` (below).
- **Two addresses inside the engine**, found in its code (below).

`setup.sh` and `tools\setup.ps1` pick the title by its engine DLL and pass
`--engine` and `--pal`; everything else above the host finds for itself.

## The libraries

Every title in the family packs its samples the same way: one archive of
concatenated `tPxD` files and an `.INF` beside it - `DANCE20` and `DANCE20.INF`,
plus a few kits (`DANCESK4.INF`...) in the same folder. Dance eJay 1's
`DMACHINE\PXD.TXT` and `MAX.TXT` index loose files instead; the host reads
either.

An `.INF` is thirteen header lines and `[SAMPLES]`, then twelve lines a sample:

```
1512        id
1           kit (always 1 in Dance eJay 2's)
"D5MA060"   short name
0           offset in the archive
4475        size
"euro"      label, line 1
"kick3"     label, line 2
1           length in bars
13          sound group
""  0  ""   unused
```

`tools/conform.py` checks, for every record of every library on the discs here:
the offset lands on a `tPxD` header, the records are the archive's members back
to back, the bars field is a block length, and each group is one category of
sound - each member's header names its category ("loop", "drum", "Kick"), and
group 1 is loops, 2 drums, 3 bass and so on through 10, in the buttons' order.
Groups 13 to 18 in Dance eJay 2 (Kick, Snare, HiHat, Cymbal, Perc, DrumFx)
belong to the groove generator, not the browser.

**The engine plays a file from its start.** `APlay` is given a file name; the
engine opens it, asks its size and reads it whole. There is no offset to give
it - arguments 4 and 5 are stored in the track entry and copied into the voice,
but never reach a file call - and handing it the archive plays the archive from
its first sample. Each member is a complete `tPxD` file, so the host copies a
member out the first time it is placed, to a cache folder (`%TEMP%\ejay2`,
`~/.cache/ejay2`, `~/Library/Caches/ejay2`), and hands the engine that.
Dancejay presumably does the same: its placement sites pass names from a list
of its own, and the engine exports `RWavToTemp`.

### The tempo is the title's

A bar is four beats at the title's tempo, and the engine counts output bytes,
176,400 a second. Dance eJay 2 is 140 BPM - 302,400 to the bar, measured long
before this - and HipHop eJay 2 is 90, 470,400. The samples say which: a
member's header carries its length in eighths of an engine unit (an eJay 1
header's field is in halves), so length / bars over a whole `.INF` has one clear
mode - 302,400 for 455 of Dance eJay 2's one-bar loops, 470,400 for 3,052 of
HipHop eJay 2's. The host takes the bar from the library it loads. Before that,
HipHop's blocks were placed on Dance-length bars and cut short.

`conform.py` likewise finds each disc's tempo, as the one that makes most of its
loose samples a whole number of beats: Dance eJay 1 and the Superpack 140, Rave
eJay 1 180.

## Two addresses inside the engine

The host reads two of the engine's own variables. Every title's engine is a
separate build, so the addresses differ; each is found from the code that uses
it, the same shape in every engine so far:

| | Found as | D4 | H4 | R4 | D5 |
|---|---|---|---|---|---|
| track records | `APlay`: `imul eax, eax, 0x84; add eax, <tracks>` | `0x100429C8` | `0x100A95D8` | `0x10043B38` | `0x10095108` |
| mixer armed | `RTimer`'s first call, the mixer tick: `movsx r32, word [flag]; test r32, r32; jle` | `0x1003AC68` | `0x10098500` | `0x1003BDD8` | `0x10083528` |

A track record is `0x84` bytes: entry array at `+0x64`, cursor at `+0x68`,
count at `+0x6A`. A track holds 30 entries (`APlay` wraps the count at 30).

## Dance eJay 3

Everything draws: the darker workspace, its 16 lanes with their REC, SOLO and
MUTE strips, its buttons, the browser with its own library. But `AGetTime`
stays at 0 and the transport never moves, so nothing plays. (On Windows its
mixer-armed flag never goes up either; under win32hle it does, and the
transport still does not move.) The engine
has two exports the others do not, `ARegDelete` and `ARegUnIn`, which suggests
a registration step; Dance eJay 3's front end also leans on a dozen OCX controls
the others do not have. Setup recognises it and says it is not supported yet.
