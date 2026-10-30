#!/usr/bin/env python3
"""Reapply manual corrections to the dsd config after `dsd init` regenerates it.

Usage: python tools/dsd/apply_fixups.py <version>      e.g. w2

Each fixup is idempotent, so running this twice changes nothing.
"""
import re, struct, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# Words in a data table of {pointer, count} pairs that `dsd init` took for 2-byte Thumb functions,
# decoding the pointer as an ARM branch: (overlay, address). The pointer is read from the ROM.
DATA_POINTERS = {
    'w2': [
        (113, 0x021ee6a8), (113, 0x021ee6b0), (113, 0x021ee6b8),
        (114, 0x021ee5b8),
        (119, 0x021ee734), (119, 0x021ee73c),
    ],
}

# Relocations whose target init got wrong: (overlay, from, old target, new target).
# ov036 loads a Thumb function pointer; the label at 0x021ee3fc is Thumb, so the linker sets bit 0 itself.
RETARGETS = {
    'w2': [
        (36, 0x021d0be8, 'to:0x021ee3fd', 'to:0x021ee3fc'),
        (36, 0x021d0be8, 'to:0x021ee3fc add:0x1', 'to:0x021ee3fc'),
    ],
}


def overlay_info(version):
    text = (ROOT / f'extract/{version}/arm9_overlays/overlays.yaml').read_text()
    info = {}
    for block in text.split('\n  - ')[1:]:
        ov = int(re.search(r'id: (\d+)', block).group(1))
        base = int(re.search(r'base_address: (\d+)', block).group(1))
        name = re.search(r'file_name: (\S+)', block).group(1)
        info[ov] = (base, ROOT / f'extract/{version}/arm9_overlays' / name)
    return info


def edit_lines(path, fn):
    lines = path.read_text().split('\n')
    new = fn(lines)
    if new != lines:
        path.write_text('\n'.join(new))
        return True
    return False


def main(version):
    config = ROOT / f'config/{version}/arm9/overlays'
    info = overlay_info(version)
    changed = 0

    for ov, addr in DATA_POINTERS.get(version, []):
        base, binary = info[ov]
        target = struct.unpack_from('<I', binary.read_bytes(), addr - base)[0]
        a, t = f'0x{addr:08x}', f'0x{target:08x}'
        reloc = f'from:{a} kind:load to:{t} module:overlay({ov})'
        changed += edit_lines(config / f'ov{ov:03d}/relocs.txt',
                              lambda ls: [reloc if l.startswith(f'from:{a} ') else l for l in ls])

        def fix_symbols(ls):
            out = [f'data_ov{ov:03d}_{addr:08x} kind:data(any) addr:{a}'
                   if l.endswith(f' addr:{a}') and 'kind:function' in l else l for l in ls]
            if not any(f' addr:{t}' in l for l in out):
                out.insert(len(out) - (out[-1] == ''), f'data_ov{ov:03d}_{target:08x} kind:data(any) addr:{t}')
            return out
        changed += edit_lines(config / f'ov{ov:03d}/symbols.txt', fix_symbols)

    for ov, addr, old, new in RETARGETS.get(version, []):
        a = f'from:0x{addr:08x} '
        changed += edit_lines(config / f'ov{ov:03d}/relocs.txt',
                              lambda ls: [l.replace(old, new) if l.startswith(a) and f'{old} ' in l + ' ' else l
                                          for l in ls])

    print(f'{version}: {changed} file(s) changed')


if __name__ == '__main__':
    main(sys.argv[1])
