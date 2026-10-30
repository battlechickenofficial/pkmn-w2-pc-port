# dsd for B2W2

Released dsd (0.12.1) can't handle DSi-enhanced (TWL hybrid) games like B2W2. Use the `dsi-hybrid` fork, which pins its own ds-rom fork.

## Build (Windows, needs Rust with MSVC)
```
git clone -b dsi-hybrid https://github.com/fuddlesworth/ds-decomp
cd ds-decomp
git checkout 2dc1b27          # version used here; pins fuddlesworth/ds-rom @ 66e83ef
cargo build --profile release-fast -p ds-decomp-cli
copy target\release-fast\dsd.exe <repo>\tools\bin\dsd.exe
```

## Pipeline (W2)
```
tools/bin/dsd.exe rom extract --rom baserom/baserom_w2.nds --output-path extract/w2
tools/bin/dsd.exe rom build   --config extract/w2/config.yaml --rom build/dsd_roundtrip_w2.nds   # byte-identical
tools/bin/dsd.exe init        --rom-config extract/w2/config.yaml --output-path config/w2 --build-path build/w2
tools/bin/dsd.exe delink      --config-path config/w2/arm9/config.yaml
tools/bin/dsd.exe dis         --config-path config/w2/arm9/config.yaml --asm-path build/w2/asm
```
`init` regenerates `config/w2/` and overwrites manual changes, so all corrections live in `apply_fixups.py`:
```
python tools/dsd/apply_fixups.py w2      # idempotent, rerun after every init
```

## Build and verify
```
python configure.py w2
tools/bin/ninja.exe        # delink -> lcf -> mwldarm (dsi/1.1) -> rom config -> check modules -> rom build -> SHA-1
```

## Fixups (in apply_fixups.py)
- ov036 `0x021d0be8` loads a Thumb function pointer into overlays 113/123/127/131. It needs `to:0x021ee3fc`. The label is Thumb, so the linker sets bit 0 itself; adding `add:0x1` on top makes it +2.
- ov113/114/119: six words in `{pointer, count}` tables were taken for 2-byte Thumb functions with ARM branches. They become `kind:load` relocs to the stored pointer, with data symbols.
