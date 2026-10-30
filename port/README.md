# Windows port (static recompilation)

The ARM9 code of White 2 (JP) is translated into C ahead of time and compiled with MSVC. A runtime stands in for
the DS hardware and the ARM7. Nothing is emulated instruction by instruction: each guest function becomes one C
function.

## Build and run
```
cd tools/recomp && cargo build --release && cd ../..
python port/build.py w2                    # recompile + MSVC -> build/w2/port/b2w2.exe (about 20 s)
cd build/w2/port
b2w2.exe ../../../baserom/baserom_w2.nds   # writes b2w2.log: first access to each I/O register, PXI, card
```
Needs Visual Studio 2022 with the C++ workload, Rust, and the dsd config/extract from `tools/dsd/README.md`.

## Pieces
| Path | What |
|---|---|
| `tools/recomp/` | Recompiler (Rust, on unarm, the decoder dsd uses). Reads `extract/w2` binaries and the function symbols in `config/w2`, follows each function's control flow from its start, and writes `build/w2/port/gen/*.c`, prototypes, and per-module address→function tables. `recomp dis <file> <base> <start> <end> arm\|thumb` disassembles code no config covers (the ARM7, copied code). |
| `port/runtime/runtime.{h,c}` | Guest CPU state (registers, NZCVQ, banked modes), memory through a 4 KB page table (main RAM, WRAM, TCMs as set by CP15, VRAM, palette, OAM), I/O registers, CP15, PSR, BIOS SWIs, dispatch for indirect calls, boot. |
| `port/runtime/arm7.c` | High-level stand-in for the ARM7. Each behaviour cites the code it mimics. |
| `port/runtime/card.c` | Game card bus: ROM reads (`B7`) and chip ID (`B8`) from the ROM file, plus card-timed DMA. The game's own file-system code loads files and overlays through it. |
| `port/code_copies.txt` | Code the game copies before running it, which gets recompiled at its run address. |

## How the translation works
- **Calls:** `bl`/`blx` become direct C calls when the target is known at recompile time, and `rc_call` (a table lookup) otherwise. A call to the middle of a function gets an entry of its own.
- **Branches:** branches inside a function become `goto`. A branch out of the function is a tail call followed by `return`. ARM (`addls pc, pc, rX, lsl #2`) and Thumb (16-bit offset tables) jump tables become `switch`es.
- **Returns:** `bx lr`, `pop {pc}`, `ldm sp!, {..pc}`, and any computed jump to the function's own return address (`mov r12, lr` … `bx r12`) all become C `return`.
- **Watchdog:** every backward branch decrements a budget. When it runs out, `rc_tick` advances simulated scanlines, runs the ARM7 stand-in, and reports a loop that never ends.
- **Copied code:** a code copy is used only while guest memory at its run address still holds the bytes it was recompiled from.

## Status (2026-10-30)
- **Recompiled:** ARM9 main, ITCM, DTCM and autoloads. That's 219,642 instructions in 7,794 functions/entries, with no unsupported instructions. Overlays aren't compiled in yet (`recomp w2 <out> all` generates them).
- **Boot runs through:**
  - crt0: TCM setup and the boot handshake with the ARM7.
  - SDK init: PXI sync, the shared-RAM CPU sync, and PXI handler registration.
  - The first PXI message (tag 13).
  - Divider and square-root contexts.
  - Card reads from the ROM.
  - Power on and display/VRAM register setup.
- **Stops at** the first thread wait. `func_0206fe3c` sleeps the main thread (`OS_SleepThread` = `func_0207a574`) until an interrupt callback sets a flag, and no interrupt is ever delivered.

## Next
1. **Interrupts.** Raise IF bits from the scanline counter (VBlank, VCount), timers, card and IPC. At safe points (`rc_tick`, `IntrWait`/`Halt`), when IME, IE & IF and CPSR.I allow it, enter IRQ mode and call the handler whose address is in DTCM + 0x3FFC, as the BIOS does.
2. **Threads.** The SDK switches threads by saving registers into the thread struct and loading another thread's. Give each guest thread a host fiber, and turn the SDK's context save/load into fiber switches.
3. **ARM7 PXI services.** Answer the tags the game sends (OS, power, touch, RTC, sound, …) in `arm7.c`.
4. **Overlays.** Compile all overlays, and track which are loaded. One option is to hook the SDK's overlay load; the other is to check memory, the way code copies are checked.
5. **Graphics.** Render the 2D engines and the 3D GPU (GX FIFO) from VRAM/OAM/registers through SDL + OpenGL, and map VRAM banks properly from VRAMCNT.
6. **Sound and input.** Mix the SDAT sequences, and map keys and touch to SDL input.
