# B2W2 (JP) decomp → Windows build: roadmap

Status as of 2026-10-30. Targets: Pocket Monsters Black 2 / White 2, Japan, Rev 1 (IREJ / IRDJ).

## Stage 0: done

| Item | Result |
|---|---|
| ROM validation | All 4 Gen V JP ROMs pass header and logo CRC; every overlay and NARC parses (one exception, see below) |
| Extraction | `tools/ndsrom.py extract`: header, ARM9/ARM7, 344 ARM9 overlays, banner, DSi ARM9i/ARM7i, digest tables, full `data/` tree |
| Decompression | ARM9 static and all overlays BLZ-decompressed to `extracted/<game>/decomp/` (sizes verified against overlay table RAM sizes) |
| Matching rebuild | `ndsrom.py verify`: B2, W2, Black and White all reassemble **byte-identical** (SHA-1) from the extracted tree |
| Assets | NARCs unpacked for all four games (`extracted/<game>/narc_unpacked.zip`) |

### Baserom SHA-1s (matching targets)

| Game | Code | SHA-1 | Note |
|---|---|---|---|
| Black 2 Rev1 | IREJ | `772786652fa56e70f243868e601a2a5093985b9c` | trimmed to 0x135EC400 bytes; see dump issues |
| White 2 Rev1 | IRDJ | `01666f87838c57dc0a65930a916982ad8941d8fb` | full 512 MiB image |
| Black | IRBJ | `e5db5b8f79e8ae819be7d60e9e1006221d80ffab` | overlay 12 damaged |
| White | IRAJ | `e392548c1a59e4cafd7ba7a502770369410b2358` | clean |

### Dump issues found
- **Black 2 `[b]`**: game data is intact, but the 202 MB after the used-ROM size contains a stray copy of earlier ROM bytes instead of 0xFF padding. The decomp therefore targets the trimmed image, which is what the game actually reads. A clean redump would allow a full-image match.
- **Black (IRBJ)**: ARM9 overlay 12 (RAM 0x02172D20, 9084 bytes compressed) does not BLZ-decompress. Its footer is valid, so a byte in the body is probably corrupted. Redump before relying on Black's code. Its assets are unaffected.
- **Black 2: the DSi area is bad too**, so this was probably dumped in DS mode. From ARM9i+0x4000 onward, all of ARM7i and the region before the ARM9i are one block repeated, and the header's ARM7i HMAC doesn't match. The banner HMAC, using the same key, does. W2's DSi area verifies. B2's DS-mode code and data (ARM9, overlays, files) are unaffected, but a matching B2 build needs a redump made in DSi mode, for example with GodMode9i on a DSi or 3DS.
- The Black file is labelled `5216`, which is the release number on the White file too. Worth checking where that dump came from.

## Key numbers
- ARM9 static: 471,512 bytes decompressed. ARM9 overlays: 344, 3.69 MB of code/data. ARM7 is stock Nitro SDK (identical across all four games, 0x28F84 bytes).
- B2 vs W2: 83/344 overlays are byte-identical, and most of the rest differ mainly through address shifts. 309/317 data files are identical. **One source tree with a version define** is the right structure.
- BW1 vs BW2: 244 data paths share a name, but only **20** have identical content. The numbered `a/x/y/z` archives are reshuffled between generations.

## Stage 1: disassembly and delinking
Two routes were considered:

**A. Recommended: join or fork `fuddlesworth/pokebw2`.** A public WIP decomp of B2W2 already exists. It rebuilds the USA/Europe ROMs byte-for-byte from one source tree, is GPLv3, and states a native PC port as its long-term goal. Its build uses Python 3.11+, ninja and LLVM on Linux/WSL/macOS, and downloads the rest of its tools on first run. Adding **JP Rev1 as another version config** reuses every function already matched there. The JP work is then mostly symbol/address maps plus JP-specific text and font differences, instead of decompiling ~4 MB of code from scratch.

**B. Independent:** `ds-decomp` (dsd) to auto-split ARM9 and overlays into delinked objects, Metrowerks `mwccarm` (CodeWarrior for NITRO) as the matching compiler, and objdiff for per-function diffs. `tools/ndsrom.py` stays as the ROM pack/verify step.

**Status (2026-10-30): route B chosen. White 2 links from delinked objects and rebuilds byte-identical** (`python configure.py w2` then `tools/bin/ninja.exe`, about 14 s).

Toolchain (see `tools/dsd/README.md`): Rust 1.99.1 (MSVC). `tools/bin/dsd.exe` is built from `fuddlesworth/ds-decomp` branch `dsi-hybrid` @ `2dc1b27`, which pins `fuddlesworth/ds-rom` @ `66e83ef`. objdiff-cli v3.8.3 is in `tools/bin/`. Released dsd (0.12.1) cannot handle B2W2: there's no ARM9 footer, the autoload entries are 16 bytes, static initializers run from a TWL init table in DTCM rather than a `.ctor` section, and there's the ARM9i LTD module. The `dsi-hybrid` fork handles all of these.

| Step | W2 (IRDJ) | B2 (IREJ) |
|---|---|---|
| `dsd rom extract` → `extract/<g>/` | OK, ARM9i split into `ltd_autoload_0` | fails: ARM9i doesn't decompress (bad DSi area, see dump issues) |
| `dsd rom build` round-trip | **byte-identical**, SHA-1 `01666f87…` | blocked |
| `dsd init` → `config/<g>/` | OK: 56,839 symbols, 43,389 functions, 224 warnings | blocked |
| `dsd delink` → `build/<g>/delinks/` | OK: 267 objects, after `tools/dsd/apply_fixups.py` corrects 1 relocation target and 6 data pointers that init took for Thumb functions | blocked |
| `dsd dis` → `build/<g>/asm/` | OK: 267 files, 47 MB | blocked |
| `mwldarm` (dsi/1.1) link → `dsd rom config` → `rom build` | **byte-identical**; `dsd check modules` passes every module | blocked |

**W2 is the primary target until B2 is redumped.** B2 then becomes a second version config with the same layout, which will also tell us how much the two games share.

Next:
1. `dsd objdiff` → `objdiff.json`, with the compiler flags pokebw2 settled on (`mwccarm` dsi/1.1p1 for game code; the SDK libraries use their own versions), so each function can be matched one at a time.
2. Identify the SDK, NitroSystem and libc code with `dsd sig apply`, and name GFL symbols. pokebw2's USA symbols (GPLv3) can be ported by matching functions.
3. Add the compile step to `configure.py` (`src/` → `mwccarm` → objects that replace their delinked counterparts), so the first C file can go in.

Either way, rebuilding matching ROMs also needs BLZ **re-compression** that reproduces Nintendo's byte output. Until that exists, keep the original compressed blobs for unmodified modules.

## Stage 2: matching decompilation
Order by payoff: Nitro SDK and libc (largely shared with other DS decomps) → GameFreak "GFL" system layer (heap, FS, tasks, messages) → field and overworld → battle engine → menus → minigames, PWT, Join Avenue, Musical. Expect years of community effort. Route A cuts most of that.

## Stage 3: Windows build: static recompilation (in progress)
Chosen route (2026-10-30): static recompilation instead of waiting for matching C. See [port/README.md](../port/README.md).
- `tools/recomp` turns the dsd-described ARM9 modules into C (219,642 instructions, no unsupported ones). `port/build.py` compiles them with MSVC into `b2w2.exe`.
- The runtime boots White 2 through crt0, SDK OS/PXI init and card reads up to the first thread wait.
- Next: interrupts, guest threads on host fibers, ARM7 PXI services, overlays, then graphics, sound and input through SDL/OpenGL.

Matching decompilation (Stage 2) can continue alongside: a decompiled function can replace its recompiled counterpart one at a time.

## Stage 4: merging BW1 assets into the B2W2 build
Because archive numbers don't line up between generations, merging needs a semantic map:
1. Build a table per archive: BW1 `a/x/y/z` → content type → BW2 `a/x/y/z` (maps, encounters, trainers, scripts, text banks, graphics, sound).
2. Write converters where formats changed between generations (e.g. trainer, encounter and zone headers). BW1-only files: `gfl_font.dat`, `titledemo.narc`, `wb_sound_data.sdat`.
3. Implement a **relayout** mode in `ndsrom.py rebuild` (currently files must keep their size) so modified or added assets can be packed into a ROM, or load them directly from disk in the PC build.

## Housekeeping
- The repo lives in OneDrive. `extracted/` (~1.4 GB) and `baserom/` (1 GB) will sync to the cloud. Consider moving the working copy outside OneDrive.
- `.gitignore` excludes all game data. Never push ROM-derived files.
