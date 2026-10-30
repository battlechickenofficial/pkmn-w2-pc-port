# b2w2Decmop

Decompilation workspace for Pocket Monsters Black 2 / White 2 (Japan, Rev 1), aimed at a native Windows build.
Black / White (Japan) assets are extracted alongside for a later merge.

**Current stage:** White 2 links from dsd-delinked objects with Metrowerks `mwldarm` and rebuilds byte-identical. The Windows port (static recompilation, [port/](port/README.md)) boots the recompiled ARM9 up to the first thread wait. Black 2 is waiting on a DSi-mode redump. See [docs/ROADMAP.md](docs/ROADMAP.md).

## Layout
```
baserom/            your dumps: baserom_b2.nds, baserom_w2.nds   (git-ignored)
extracted/<game>/   generated split of each ROM                  (git-ignored)
  rom/              header, arm9/arm7, overlays, banner, DSi sections, gap blobs
  decomp/           BLZ-decompressed ARM9 + overlays (input for disassembly)
  data/             the game's filesystem
  narc_unpacked.zip every NARC unpacked
  layout.json       every byte of the ROM -> source file; overlay table; module params
extract/<game>/     dsd's ROM extract (rom build input)             (git-ignored)
config/<game>/      dsd config: symbols, relocs, delinks (source, committed)
build/<game>/       delinked objects, asm                          (git-ignored)
tools/dsd/          how to build dsd for B2W2, pipeline, manual config fixes
tools/ndsrom.py     dependency-free ROM info / extract / rebuild / verify (+BLZ, NARC)
tools/bin/          downloaded toolchain: dsd, objdiff-cli, ninja  (git-ignored)
tools/mwccarm/      Metrowerks compilers/linkers, all versions     (git-ignored)
configure.py        writes build.ninja for the matching build
tools/recomp/       ARM9 -> C static recompiler (Rust)
port/               Windows port: runtime, ARM7 stand-in, card bus, build script
```

## Matching build (W2)
```
python configure.py w2
tools/bin/ninja.exe
```
To set up the toolchain from scratch, see [tools/dsd/README.md](tools/dsd/README.md).

## ROM tool (any shell with Python 3)
```
python tools/ndsrom.py info baserom/baserom_b2.nds                  # header, CRCs, structural checks
python tools/ndsrom.py extract baserom/baserom_b2.nds extracted/b2 --narcs
python tools/ndsrom.py verify baserom/baserom_b2.nds extracted/b2   # rebuild and compare SHA-1
```
