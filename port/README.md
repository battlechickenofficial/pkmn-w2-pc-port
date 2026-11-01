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

**Display:** F11 or Alt+Enter = fullscreen, F10 = layout (top screen only, the default / stacked / big top screen
with the touch screen inset / side by side), F9 = widescreen on/off. The window can be resized; the picture keeps its aspect ratio.
**Widescreen** (on by default) draws the top screen 342 pixels wide (16:9) at the same scale. The 3D world and
rotated/scaled backgrounds carry on into the extra width. Tiled backgrounds repeat into it when the line is a
repeating pattern (grids, checks, plain colour). On the backmost layer, a picture that crosses the screen edge
(more than two colours down the edge column) is mirrored outward, so the title menu's Poke Ball closes up round.
Anything else stretches its edge pixel outward, so a text box never wraps into the opposite margin. Windows, blending and fades cover the margins. Sprites stay in the middle 256 pixels
(games park unused ones just off screen). The game still culls 3D objects it thinks are off screen, so some can pop
in at the edges.

**Top screen only** (the default layout) brings what the game shows on its touch screen onto the top screen, so
everything is played with the buttons:
- Battles: the view stands further back and higher (the 3D picture scaled to 80% and raised, the HP boxes raised
  with it), leaving the bottom for the buttons.
- Battle commands: the text box makes way for a row of four buttons, FIGHT, POKÉMON, BAG, RUN, all one symmetric
  shape after the game's BAG button (a tab with the Japanese word, a black bar with the English one between
  chevrons, the tab's mirror below), in each button's colours as the game draws them and with its own words. The
  one the game's cursor is on grows, pushing the others aside, with the cursor around it. Left and Right move along
  the row and round from RUN to FIGHT; presses are queued, and where the game's own layout needs two presses the
  second one's cursor sound is hushed.
- Battle moves: two rows of two plates in the same place, each with the move's type, name and PP; the D-pad moves
  as in the game.
- Party balls: under each side's HP box, all at the small size (the one in battle shrunk to match).
- The battle bag and party (over the battle, dimmed; also the party after a faint, to send the next one out), and the
  field menu: the whole screen without its background.
- While the game has its main engine on the touch screen (the party list from the field menu), the touch screen is
  shown in the top screen's place.
- The touch screen's status bar (clock, wireless, battery) is left out. Its fades become fades to transparent.
Screens are known by a function of theirs that the game runs every frame (`runtime.c`, `scene_marks`). Screens not
handled yet stay on the touch screen: use F10 for a layout that shows it.

**Save files:** the save chip is one of the files in `saves/`; `saves/current.txt` names the one in use. On the title
menu, the game's own つづきから はじめる opens the list of files to continue from (without a save, an extra item in the
same place does). The list is on the top screen in the game's font and style: the files on the left with a picture
each, on the right the selected file's report panel from its last save (date, place, party, badges, Pokédex, play
time), and under it the file's picture and date. Up/Down, L/R a page, A to choose, B to go back. The file in use
continues at once; another restarts the game on it and continues; あたらしく はじめる starts on an empty chip. F7
opens the same list anywhere (unsaved progress is lost). The field menu's レポート opens the list at once, and choosing a
file starts writing to it straight away: the game's report event skips its question and はい/いいえ (`fe_report_event`),
and nothing of it shows on the top screen but the notebook over the player (B closes the list and writes nothing). A save started any other
way pauses on "writing to the report" with the same list. The whole chip is written to the chosen file, so saving to
another or a new one copies the game as it is now. A new game's first write, during the introduction, isn't asked
about. Each file keeps `<name>.png` (the field's 3D view as saved, without text boxes) and `<name>.report.png` (the
report panel). The game's kana and kanji modes are both understood. To try someone else's save, put their 512 KB
`.sav` from White 2 or Black 2 in `saves/`; Black/White saves are listed but can't be loaded. An older `b2w2.sav`
becomes `Save 1` (レポート 1).

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
| `B2W2_PICK=<name>[,<name>...]` | headless: the save file the list chooses (one after another each time) |
| `B2W2_LAYOUT=n` | start in layout n (0 stacked, 1 big top, 2 side by side, 3 top only) |
| `B2W2_CLOCK=<unix time>` | the clock starts there and runs with the frames, so runs repeat exactly |
| `B2W2_ONLY_A=mask`, `B2W2_ONLY_B=mask` | draw only these layers of an engine (bits 0-3 BG0-BG3, 16 sprites) |
| `B2W2_LOG_SCENES=1` | log when each known screen comes up |
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
| `port/runtime/topscreen.c` | The top-screen-only layout: the touch screen's parts placed on the top screen per screen, the battle command and move rows, party balls, key translation. |
| `port/runtime/font.c` | The game's 12x15 menu font, read from the ROM (`a/0/2/3`), for what the runtime draws. |
| `port/code_copies.txt` | Code the game copies before running it, recompiled at its run address. |

## How the translation works
- **Calls:** `bl`/`blx` become direct C calls when the target is known at recompile time, `rc_call` (a table lookup) otherwise. A call to the middle of a function gets an entry of its own. A Thumb `bl` to another point of the same function is a long jump (`goto`), and a `bl` whose callee returns to our own caller was a branch into shared code, so we return too.
- **Branches:** inside a function they become `goto`; out of it, a tail call and `return`. ARM and Thumb jump tables become `switch`es; `add pc, pc, rX, lsl #2` that isn't a row of branches (libc's unrolled division) can land on any code word after it.
- **Returns:** `bx lr`, `pop {pc}`, `ldm sp!, {..pc}`, and any computed jump to the function's own return address. Each return records where it went (`ret_to`): after an indirect tail call made with a changed `lr`, execution continues there.
- **Time:** each 256 backward branches advance the clock 1024 bus cycles; Halt/IntrWait jump to the next interrupt.
- **Interrupts** run the game's handler (DTCM+0x3FFC) on a BIOS-style frame. **Threads:** after every exception return (the SDK's `OS_LoadContext` or its IRQ-exit thread switch) the runtime reads the SDK's current-thread pointer and switches to that thread's fiber, creating it at the thread's start the first time.
- **Overlays and copied code** apply while guest memory holds the bytes they were compiled from (overlay start and the called function; the function alone for DS Protect's partly encrypted overlay 337).
- **DS Protect:** its call wrappers (`dsprot=0x94` in the symbols) check the card and hardware, then call the function they're given. The checks can't pass recompiled (they hash and decrypt code that isn't what runs), and failing them ends the game, so when a wrapper is given a function the runtime calls it directly (`rc_dsprot_call`). Without this, closing the party screen after reordering crashed: its cleanup ran twice.
- **Infrared chip:** before a battle gives experience, overlay 338 asks the card's infrared chip for its ID over the card's SPI bus (command 08h); a genuine card answers AAh, and anything else means no experience. The runtime answers as the chip (`card_spi_write`).
- **Fatal errors** go to `b2w2.log` too, with the stack's top words and the last overlay called.

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
