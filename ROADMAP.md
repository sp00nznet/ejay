# Roadmap

Two programs, one engine. Dance eJay 2's host is the furthest along because
its DLLs are 32-bit and can be driven directly; Dance eJay 1's engine is 16-bit
and has to be lifted before anything can drive it at all.

## Now

- **Dance eJay 2 - `.MIX` loading.** The disc ships thirteen saved arrangements
  and none of them can be opened yet. The header, the per-track settings and the
  sample-library table are decoded (seven libraries, sample ids 2000..7199); the
  block list after it is delta-coded and is not read yet. This is what "play one
  of the other songs" needs.
- **Dance eJay 2 - output latency.** Beats land within 33-119ms of the playhead,
  measured at the endpoint with `--onset`. Host tick quantisation is +-31ms of
  that on its own, so the residual is small but not zero and there is no
  calibration knob for it yet.
- **Dance eJay 1 - the sampler path.** The engine initialises, decodes its intro
  and streams it, but placing samples on the timeline still produces silence.
  `docs/ENGINE.md` has where it stops.

## Next

- **A front end worth using.** The host is a command line with a window attached:
  every arrangement is either generated or clicked together by hand, and there is
  no transport, no save, no load. See "Interface" below.
- **Sound device selection.** `ADevice` takes an argument and ignores it - the
  engine enumerates and takes the first device that reports 44.1kHz 16-bit
  stereo, so neither eJay exposes a device picker. The host can add one by
  filtering `waveOutGetDevCaps` through the DLL's import table, which is the same
  mechanism already used to count its GDI calls.
- **More sibling titles.** HipHop eJay 2 already runs - see the README. The
  same three flags should reach Techno eJay, Rave eJay and the rest of the
  1999-2002 run; what is not known is how far back the layout format goes, since
  Rave eJay is 1997 and shares the era of Dance eJay 1 rather than 2.

## Deferred

- The horizontal scroll. eJay's grid is a sixteen-bar window onto a longer song
  (`K_SPUR_HSCROLL`), and the host only ever shows the first sixteen bars.
- Recording, effects, and the mixer page. `ARec*`, `REffekt` and `RWave*` are
  named in the engine and nothing here calls them.
- Resolutions above 640x480. `K_800`, `K_1024`, `K_1152` and `K_1280` parse
  already; only the 640 art set is wired up.

## Interface

The host is driven by flags, which suits measurement and suits nothing else.
Two ways out, and they are not equivalent:

- **Keep the engine's own window** and add the transport, a bar ruler and
  load/save on top of it. eJay's artwork and control table already describe
  every button, including its rolled-over and pressed frames - the work is
  wiring them to actions, not drawing them. Nothing new to depend on, and the
  program keeps looking like itself.
- **Render to SDL** instead of GDI. Buys resizing, a frame clock and portability,
  and costs the `PXD32CL1.DLL` drawing path: the graphics DLL blits into a device
  context and it is the thing under test. It would have to draw into a DIB that
  SDL then uploads, which is a texture copy per frame and a second window system
  to be wrong about.

The first is the plan. SDL is worth revisiting if this ever needs to run
somewhere without GDI - which, for a host whose whole purpose is loading two
1999 Windows DLLs, is not soon.

## Out of scope

- Redistributing anything off either disc. The tool ships; the discs do not.
- The other eJay product lines that do not share this engine.
