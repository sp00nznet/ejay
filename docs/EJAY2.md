# Dance eJay 2 engine: PXD32D4.DLL call signatures

Argument sizes read out of the binary rather than guessed: a `__stdcall`
callee ends `ret N`, so N is the argument bytes. This matters - `AInit` and
`DPlayFile` take **no** arguments, and passing one leaks four bytes of stack
per call because the callee never pops it, which is how the first host here
segfaulted.

| export | args | export | args | export | args |
|---|---:|---|---:|---|---:|
| `ABilder` | 28 | `ABildpos` | 4 | `AClose` | 4 |
| `ACloseAll` | 0 | `ADSoff` | 0 | `ADevice` | 4 |
| `AEnd` | 0 | `AExit` | 0 | `AExport` | 8 |
| `AFenster` | 4 | `AGetFree` | 4 | `AGetFull` | 4 |
| `AGetInput` | 0 | `AGetString` | 8 | `AGetTime` | 4 |
| `AInit` | 0 | `ALautGet` | 4 | `ALautSet` | 4 |
| `ALoad` | 0 | `AMemory` | 0 | `AMitte` | 8 |
| `ANummer` | 0 | `APlay` | 48 | `APos` | 4 |
| `ARecDuplex` | 4 | `ARecInit` | 4 | `ARecInput` | 4 |
| `ARecPegel` | 4 | `ARecPlay` | 0 | `ARecStart` | 0 |
| `ARecStop` | 0 | `ARecTest` | 4 | `ARecTimer` | 0 |
| `ASelectMic` | 0 | `ASetCur` | 4 | `ASetFader` | 8 |
| `ASetPfad` | 4 | `ASetPitch` | 4 | `ASortIn` | 0 |
| `ASortInit` | 0 | `ASortOut` | 0 | `ASortStart` | 0 |
| `ASortStart2` | 0 | `AStart` | 4 | `AStop` | 0 |
| `ATest` | 0 | `ATimer` | 0 | `ATyp` | 4 |
| `AVbInfoCall` | 0 | `AWaveDauer` | 4 | `AWelle` | 12 |
| `AWellePos` | 4 | `BWaveDauer` | 0 | `DCloseAll` | 0 |
| `DGetZeit` | 4 | `DPlayFile` | 0 | `DPlayFileCheck` | 4 |
| `DPlayFileClose` | 4 | `DPlayUpdate` | 0 | `DStart` | 0 |
| `Debimem` | 0 | `Extra` | 0 | `ExtraAus` | 0 |
| `Fade` | 0 | `RDrum` | 12 | `REffekt` | 8 |
| `RGetName` | 8 | `RGetParam` | 4 | `RMenu` | 0 |
| `RRecoSave` | 4 | `RTimer` | 0 | `RWavToTemp` | 28 |
| `RWaveFilter` | 4 | `RWaveGetInfo` | 4 | `RWaveGetTime` | 0 |
| `RWaveLaden` | 0 | `RWaveParam` | 8 | `RWavePause1` | 0 |
| `RWavePause2` | 0 | `RWavePlay` | 0 | `RWaveRec` | 0 |
| `RWaveSave` | 0 | `RWaveSetInfo` | 4 | `RWaveStop` | 0 |
| `RWaveTakt` | 0 | `RWaveTransfer` | 4 | `RpDrum` | 0 |

87 exports. 27 of them carry the same names as the 1997 engine.

Widths doubled in the port, as expected from 16- to 32-bit: `APlay` is 48
bytes here against 24 in `DANCE02.DLL`. Others are unchanged - `ABilder` is
28 in both, `ATimer` and `ASortStart` take nothing in both.
