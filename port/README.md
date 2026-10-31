# Windows port (static recompilation)

The ARM9 code of White 2 (JP) is translated into C ahead of time and compiled with MSVC. A runtime stands in for
the DS hardware and the ARM7. Nothing is emulated instruction by instruction: each guest function becomes one C
function.

## Build and run
```
cd tools/recomp && cargo build --release && cd ../..
python port/build.py w2                    # recompile all 352 modules + MSVC -> build/w2/port/b2w2.exe (~90 s clean)
cd build/w2/port
b2w2.exe ../../../baserom/baserom_w2.nds
```
Needs Visual Studio 2022 with the C++ workload, Rust, and the dsd config/extract from `tools/dsd/README.md`.
Keeps save files in `saves/` and writes `b2w2.fw` (firmware, once the game writes to it) and `b2w2.log` next to
where it runs.

**Controls:** arrows = D-pad, X = A, Z = B, S = X, A = Y, Q = L, W = R, Enter = Start, Backspace = Select,
mouse on the touch screen = stylus, hold Tab = fast forward (as fast as it goes), F8 = speed 1x / 2x / 4x / 8x
(shown in the title bar), F7 = save files, Esc = leave fullscreen / quit.

**Display:** F11 or Alt+Enter = fullscreen, F10 = layout (stacked / big top screen with the touch screen inset /
side by side / top screen only), F9 = widescreen on/off. The window can be resized; the picture keeps its aspect ratio.
**Widescreen** (on by default) draws the top screen 342 pixels wide (16:9) at the same scale. The 3D world and
rotated/scaled backgrounds carry on into the extra width. Tiled backgrounds repeat into it when the line is a
repeating pattern (grids, checks, plain colour). On the backmost layer, a picture that crosses the screen edge
(more than two colours down the edge column) is mirrored outward, so the title menu's Poke Ball closes up round.
Anything else stretches its edge pixel outward, so a text box never wraps into the opposite margin. Windows, blending and fades cover the margins. Sprites stay in the middle 256 pixels
(games park unused ones just off screen). The game still culls 3D objects it thinks are off screen, so some can pop
in at the edges.

**Save files:** the save chip is one of the files in `saves/`; `saves/current.txt` names the one in use. The title
menu gets one more item, second in the list: つづきから はじめる (or レポートを えらぶ when the game already shows its own つづきから). It
moves, highlights and sounds like the game's own items (the game's cursor, decide and cancel sounds, which stay
loaded). A on it opens a list of the files, each with a picture, trainer and play time, the selected one larger on
the touch screen. It's in the game's font and style, and it uses the game's buttons: Up/Down, L/R a page, A to
choose, B to go back. Choosing a file restarts the game on it and presses through to the title menu, and あたらしい レポート
starts on an empty chip. F7 opens the same list anywhere (unsaved progress is lost). When the game saves, it pauses
on "writing to the report" with the same list, asking which file the save goes to (B: the one in use). The whole
chip is written there, so saving to another or a new file copies the game as it is now. A new game's first write,
during the introduction, isn't asked about. Each file's picture (`<name>.png`) is the field's 3D view as it was
saved, without text boxes. To try someone else's save, put their 512 KB `.sav` from White 2 or Black 2 in `saves/`;
Black/White saves are listed but can't be loaded. An older `b2w2.sav` becomes `Save 1` (shown as レポート 1).

**Running shoes** from the start: hold B to run from the first step. The runtime sets the game's own running-shoes
flag (system flag 2403), so saves keep it; `B2W2_NO_RUNNING_SHOES=1` leaves the game as shipped.

**Testing switches** (environment variables):

| Variable | Effect |
|---|---|
| `B2W2_HEADLESS=1` | no window, runs as fast as it can |
| `B2W2_SHOT_EVERY=n`, `B2W2_SHOT_FROM=f` | `shot_<frame>.png` of both screens every n frames (from frame f) |
| `B2W2_INPUT=4300:START,4700:A,6820:T128x150` | hold a button (or touch x,y) for 6 frames from a frame |
| `B2W2_WATCHDOG=1` (or `2`) | progress every 2 s from a host thread (`2`: also samples guest registers) |
| `B2W2_REGDUMP=1` | display registers in the log with each screenshot |
| `B2W2_WAV=out.wav` | also write the sound to a WAV file |
| `B2W2_FULLSCREEN=1` | start in fullscreen |
| `B2W2_LOG_3D=<frame>`, `B2W2_SND_STATS=<from>-<to>` | that frame's polygons; sequence events per player between two frames |
| `B2W2_LOG_NOTES=<from>-<to>`, `B2W2_SND_STATE=n` | notes started between two frames; sound players' state every n frames |
| `B2W2_DUMP_RAM=<frame>`, `B2W2_POKE=<frame>:<addr>=<byte>,...` | write main RAM to `ram_<frame>.bin`; set bytes at a frame |
| `B2W2_WATCH=<lo>-<hi>,<a1>-<a2>,<b1>-<b2>` | log main RAM bytes (offsets lo-hi, hex) read between frames b1-b2 but not a1-a2, and event flags tested |
| `B2W2_LOG_PXI=1`, `B2W2_LOG_SND=1`, `B2W2_LOG_THREADS=1`, `B2W2_LOG_KEYS=1`, `B2W2_CHECK_SP=1` | more logging |
| `RECOMP_CHECK_SP=1` (when building) | generated code checks every call leaves sp alone |

## Pieces
| Path | What |
|---|---|
| `tools/recomp/` | Recompiler (Rust, on unarm, the decoder dsd uses). Reads `extract/w2` binaries and the function symbols in `config/w2`, follows each function's control flow from its start, and writes `build/w2/port/gen/*.c`, prototypes, per-module address→function tables and the overlays' bytes. `recomp dis <file> <base> <start> <end> arm\|thumb` disassembles code no config covers. |
| `port/runtime/runtime.c` | CPU state and banked modes, memory through a 4 KB page table (main RAM, WRAM, TCMs as CP15 sets them, VRAM banks from VRAMCNT, palette, OAM), I/O registers, DMA, divider/sqrt, CP15, PSR, BIOS SWIs, dispatch for indirect calls, boot. |
| `port/runtime/sched.c` | Time (scanlines, VBlank, timers), interrupts delivered like the BIOS does, Halt/IntrWait, and guest threads on Windows fibers. |
| `port/runtime/arm7.c` | High-level stand-in for the ARM7: boot/IPC/shared-RAM handshakes, VBlank counter, PXI services (power, clock, touch, microphone, firmware NVRAM, save chip, sound commands, wireless refused as switched off). Each behaviour cites the ARM9 code it answers. |
| `port/runtime/snd.c` | The sound driver: SSEQ sequencer over the SBNK banks and SWAR wave archives in RAM, PCM8/PCM16/ADPCM/PSG/noise voices with ADSR, modulation and portamento, the channels the game drives directly (cries), alarms, the status the game reads, and a 32768 Hz stereo mixer. |
| `port/runtime/card.c` | Game card bus: ROM reads and chip ID from the ROM file, card-timed DMA. The game's own file system loads files and overlays through it. |
| `port/runtime/render.c` | Both 2D engines, drawn line by line with the registers of the moment (HBlank DMA and mid-frame changes show): text/affine/extended BGs, sprites, windows, blending, master brightness, VRAM display, display capture, the 3D layer. |
| `port/runtime/gpu3d.c` | 3D engine: geometry commands and a software rasterizer: all texture formats, lighting with the shininess table, toon/highlight, shadow polygons, translucent sorting, edge marking, fog, anti-aliasing (approximated). |
| `port/runtime/frontend.c` | Window, input, audio output (waveOut), 60 Hz pacing, screenshots, scripted input, restarting on another save file. |
| `port/runtime/saves.c` | Save files in `saves/`: which one is in use, what each holds (trainer card block, picture), and the list that picks one. |
| `port/runtime/titlemenu.c` | The title menu's extra item: found on its layers (buttons, selection, "New Game" by its words), drawn into the line being composed, moved onto with the game's own keys. |
| `port/runtime/font.c` | The game's 12x15 menu font, read from the ROM (`a/0/2/3`), for what the runtime draws. |
| `port/code_copies.txt` | Code the game copies before running it, recompiled at its run address. |

## How the translation works
- **Calls:** `bl`/`blx` become direct C calls when the target is known at recompile time, `rc_call` (a table lookup) otherwise. A call to the middle of a function gets an entry of its own. A Thumb `bl` to another point of the same function is a long jump (`goto`), and a `bl` whose callee returns to our own caller was a branch into shared code, so we return too.
- **Branches:** inside a function they become `goto`; out of it, a tail call and `return`. ARM and Thumb jump tables become `switch`es; `add pc, pc, rX, lsl #2` that isn't a row of branches (libc's unrolled division) can land on any code word after it.
- **Returns:** `bx lr`, `pop {pc}`, `ldm sp!, {..pc}`, and any computed jump to the function's own return address. Each return records where it went (`ret_to`): after an indirect tail call made with a changed `lr`, execution continues there.
- **Time:** each 256 backward branches advance the clock 1024 bus cycles; Halt/IntrWait jump to the next interrupt.
- **Interrupts** run the game's handler (DTCM+0x3FFC) on a BIOS-style frame. **Threads:** after every exception return (the SDK's `OS_LoadContext` or its IRQ-exit thread switch) the runtime reads the SDK's current-thread pointer and switches to that thread's fiber, creating it at the thread's start the first time.
- **Overlays and copied code** apply while guest memory holds the bytes they were compiled from (overlay start and the called function; the function alone for DS Protect's partly encrypted overlay 337).

## Status (2026-10-30)
- 352 modules, 42,810 functions, 1.57 million instructions recompiled; no unsupported instructions.
- Boots, plays the opening movie (2D and 3D) with music, shows the title screen (3D Kyurem with motion-blur
  capture), takes Start, runs the professor's introduction and naming, the Aspertia City cutscene, and the player's
  house (Mum's entrance cutscene included), where the player can walk. Tested headless through 48,000 frames (about 13 minutes of game time) without a
  crash.

## Not done / approximate
- **Sound** follows the SDAT format and the SDK's command layout as observed, but envelopes, volume curves and
  modulation are approximations, and sound capture (the reverb effect) is not emulated.
- **3D anti-aliasing** softens polygon edges toward what is behind them rather than computing coverage.
- **Wireless** is refused, as with wireless switched off: no C-Gear connections, trades or battles. The
  **microphone** answers but records silence.
- **Timing** is approximate (loop cost per tick); a game that relies on cycle counts may misbehave.
- Only White 2 (JP); Black 2 needs a DSi-mode redump.
