# Roadmap

Two generations of one engine. The eJay 2 family (Dance eJay 2, HipHop eJay 2,
Techno eJay) runs on Windows, Linux and macOS through `host/ejay2.c`, driving
each title's own DLLs. Dance eJay 1's engine is 16-bit and recompiled whole, and
its sampler path is still silent.

## Now

- **Dance eJay 1 - the sampler path.** The recompiled 1997 engine initialises,
  decodes its intro and streams it, but placing samples on the timeline still
  produces silence. [ENGINE](docs/ENGINE.md) has where it stops ("Why it is still
  silent"). This is the next thing worked on, and what gets eJay 1 into the
  launcher.
- **Dance eJay 3 - the mixer never arms.** It draws, with its own library, and
  `AGetTime` stays at 0. Its engine has two exports the others lack
  (`ARegDelete`, `ARegUnIn`), which suggests a registration step to find in
  `d_ejay3.exe`'s call sites with `tools/vb_declares.py`. [TITLES](docs/TITLES.md).
- **`.MIX` loading.** Every disc ships saved arrangements (13 on Dance eJay 2,
  18 on HipHop eJay 2) and none can be opened. The header, the per-track
  settings and the sample-library table are decoded; the block list after it is
  delta-coded and not read yet. This is what "play one of the songs on the disc"
  needs, and with each title's own library now loading, the ids in it can be
  resolved.

## Next

- **A front end worth using.** The host opens on a generated demo arrangement;
  blocks can be placed from the browser and dragged, and nothing else does
  anything. eJay's artwork and control table already describe every button,
  including its rolled-over and pressed frames - the work is wiring them to
  actions (transport, new, load, save), not drawing them.
- **Output latency.** Beats land 33-119 ms after the playhead, measured at the
  endpoint with `--onset` on Windows. `--latency MS` is the knob; there is no
  calibration yet, and off Windows nothing measures it.
- **A label glitch.** On Techno eJay under win32hle a block's label can carry
  another block's text: the graphics DLL's label pass leaves pixels behind that
  Windows does not. Cosmetic.
- **Apple silicon.** Built and run on macOS on x86-64. An arm64 Mac builds the
  same 64-bit way, guest memory at a fixed address, and has not been run.

## Deferred

- The horizontal scroll. eJay's grid is a sixteen-bar window onto a longer song
  (`K_SPUR_HSCROLL`), and the host only ever shows the first sixteen bars.
- The groove generator, recording, effects and the mixer page. The engine names
  them (`RDrum`, `ARec*`, `REffekt`, `RWave*`) and nothing here calls them; Dance
  eJay 2's groups 13 to 18 are the groove generator's sounds.
- Resolutions above 640x480. `K_800`, `K_1024`, `K_1152` and `K_1280` parse
  already; only the 640 art set is wired up.
- Rave eJay 1 and the Dance eJay Superpack: 32-bit, but an older engine
  (`PXD32RA`, `PXD32OA`) with no `PXD32CL1.DLL` and 23 of the calls the host
  relies on missing. They are Dance eJay 1's generation, and come after it.
- Lifting `Dancejay.exe` itself (VB5 native, 1.27 MB) with `MSVBVM50.DLL` kept
  real - pcrecomp's hybrid boundary. A rebuilt front end is the nearer route to
  something playable.

## Out of scope

- Redistributing anything off any disc. The tool ships; the discs do not.
- eJay titles that do not share this engine (House eJay and later).
