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
Writes `b2w2.sav` (save chip), `b2w2.fw` (firmware, once the game writes to it) and `b2w2.log` next to where it runs.

**Controls:** arrows = D-pad, X = A, Z = B, S = X, A = Y, Q = L, W = R, Enter = Start, Backspace = Select,
mouse on the lower screen = stylus, hold Tab = fast forward, Esc = quit.

**Testing switches** (environment variables):

| Variable | Effect |
|---|---|
| `B2W2_HEADLESS=1` | no window, runs as fast as it can |
| `B2W2_SHOT_EVERY=n` | `shot_<frame>.png` of both screens every n frames |
| `B2W2_INPUT=4300:START,4700:A,6820:T128x150` | hold a button (or touch x,y) for 6 frames from a frame |
| `B2W2_WATCHDOG=1` (or `2`) | progress every 2 s from a host thread (`2`: also samples guest registers) |
| `B2W2_REGDUMP=1` | display registers in the log with each screenshot |
| `B2W2_LOG_PXI=1`, `B2W2_LOG_THREADS=1`, `B2W2_LOG_KEYS=1`, `B2W2_CHECK_SP=1` | more logging |
| `RECOMP_CHECK_SP=1` (when building) | generated code checks every call leaves sp alone |

## Pieces
| Path | What |
|---|---|
| `tools/recomp/` | Recompiler (Rust, on unarm, the decoder dsd uses). Reads `extract/w2` binaries and the function symbols in `config/w2`, follows each function's control flow from its start, and writes `build/w2/port/gen/*.c`, prototypes, per-module address→function tables and the overlays' bytes. `recomp dis <file> <base> <start> <end> arm\|thumb` disassembles code no config covers. |
| `port/runtime/runtime.c` | CPU state and banked modes, memory through a 4 KB page table (main RAM, WRAM, TCMs as CP15 sets them, VRAM banks from VRAMCNT, palette, OAM), I/O registers, DMA, divider/sqrt, CP15, PSR, BIOS SWIs, dispatch for indirect calls, boot. |
| `port/runtime/sched.c` | Time (scanlines, VBlank, timers), interrupts delivered like the BIOS does, Halt/IntrWait, and guest threads on Windows fibers. |
| `port/runtime/arm7.c` | High-level stand-in for the ARM7: boot/IPC/shared-RAM handshakes, VBlank counter, PXI services (power, clock, touch, firmware NVRAM, save chip, sound bookkeeping). Each behaviour cites the ARM9 code it answers. |
| `port/runtime/card.c` | Game card bus: ROM reads and chip ID from the ROM file, card-timed DMA. The game's own file system loads files and overlays through it. |
| `port/runtime/render.c` | Both 2D engines: text/affine/extended BGs, sprites, windows, blending, master brightness, VRAM display, display capture, the 3D layer. |
| `port/runtime/gpu3d.c` | 3D engine: geometry commands and a software rasterizer with all texture formats. |
| `port/runtime/frontend.c` | Window, input, 60 Hz pacing, screenshots, scripted input. |
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
- Boots, plays the opening movie (2D and 3D), shows the title screen (3D Kyurem with motion-blur capture), takes
  Start, runs the professor's introduction and naming, the Aspertia City cutscene, and the player's room, where the
  player can walk. Tested headless through 48,000 frames (about 13 minutes of game time) without a crash; headless it
  runs at about 100 frames a second.

## Not done
- **Sound.** The ARM7's sound driver is only stood in for: command lists are acknowledged, nothing plays. Needs an
  SDAT sequencer (SSEQ/SBNK/SWAR) and a mixer, then a Windows audio output.
- **3D:** toon/highlight shading, fog, edge marking, anti-aliasing, translucent polygon sorting, shadow polygons.
- **2D:** mid-frame register changes (HBlank effects) aren't seen: each frame is drawn from the state at VBlank.
- **ARM7:** microphone, wireless (WM/WFC), and anything the game asks for later in the story.
- **Timing** is approximate (loop cost per tick); a game that relies on cycle counts may misbehave.
- Only White 2 (JP); Black 2 needs a DSi-mode redump.
