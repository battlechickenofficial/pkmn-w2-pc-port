#!/usr/bin/env python3
"""ndsrom.py - dependency-free Nintendo DS / DSi-enhanced ROM tool for the B2W2 decomp.

Commands:
  info     <rom>                 Print header, validate checksums and structure.
  extract  <rom> <outdir>        Split ROM into header, ARM9/ARM7 (+ decompressed copies),
                                 overlays, banner, DSi ARM9i/ARM7i, and the data/ filesystem.
                                 Writes layout.json describing every byte of the ROM.
  rebuild  <outdir> <out.nds>    Reassemble a ROM from an extracted tree using layout.json.
  verify   <rom> <outdir>        rebuild into a temp file and compare SHA-1 with the original.

Only the Python 3 standard library is used.
"""
import hashlib, json, os, struct, sys, tempfile, zlib

# ---------------------------------------------------------------- helpers
def u16(b, o): return struct.unpack_from('<H', b, o)[0]
def u32(b, o): return struct.unpack_from('<I', b, o)[0]

def crc16(data, crc=0xFFFF):
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc

def sha1_file(path, limit=None):
    h = hashlib.sha1(); c = 0
    with open(path, 'rb') as f:
        while True:
            n = 1 << 20
            if limit is not None: n = min(n, limit - c)
            if n <= 0: break
            chunk = f.read(n)
            if not chunk: break
            h.update(chunk); c += len(chunk)
    return h.hexdigest()

def crc32_file(path):
    c = 0
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            c = zlib.crc32(chunk, c)
    return c & 0xFFFFFFFF

# ---------------------------------------------------------------- BLZ (backward LZ, used by ARM9/overlays)
def blz_decompress(data):
    data = bytes(data)
    if len(data) < 8: return data
    enc_hdr = u32(data, len(data) - 8)
    inc = u32(data, len(data) - 4)
    hdr_len = enc_hdr >> 24
    enc_len = enc_hdr & 0xFFFFFF
    if inc == 0 or hdr_len < 8 or enc_len > len(data):
        raise ValueError('not BLZ compressed')
    out = bytearray(data) + bytearray(inc)
    src = len(data) - hdr_len
    end = len(data) - enc_len
    dst = len(out)
    while src > end:
        src -= 1; flags = out[src]
        for _ in range(8):
            if src <= end: break
            if flags & 0x80:
                src -= 2
                val = out[src + 1] << 8 | out[src]
                length = (val >> 12) + 3
                disp = (val & 0xFFF) + 3
                for _ in range(length):
                    dst -= 1
                    out[dst] = out[dst + disp]
            else:
                src -= 1; dst -= 1
                out[dst] = out[src]
            flags = (flags << 1) & 0xFF
    if dst != src:
        raise ValueError('BLZ size mismatch (dst=%d src=%d)' % (dst, src))
    return bytes(out)

# ---------------------------------------------------------------- header
class Header:
    def __init__(self, raw):
        self.raw = raw
        self.title = raw[0:12].rstrip(b'\0').decode('ascii', 'replace')
        self.gamecode = raw[0x0C:0x10].decode('ascii', 'replace')
        self.maker = raw[0x10:0x12].decode('ascii', 'replace')
        self.unitcode = raw[0x12]
        self.capacity = raw[0x14]
        self.version = raw[0x1E]
        (self.arm9_off, self.arm9_entry, self.arm9_ram, self.arm9_size,
         self.arm7_off, self.arm7_entry, self.arm7_ram, self.arm7_size,
         self.fnt_off, self.fnt_size, self.fat_off, self.fat_size,
         self.ovt9_off, self.ovt9_size, self.ovt7_off, self.ovt7_size) = struct.unpack_from('<16I', raw, 0x20)
        self.banner_off = u32(raw, 0x68)
        self.used_size = u32(raw, 0x80)
        self.header_size = u32(raw, 0x84)
        self.logo_crc = u16(raw, 0x15C)
        self.header_crc = u16(raw, 0x15E)
        self.is_dsi = self.unitcode & 2 != 0
        if self.is_dsi:
            self.arm9i_off, _, self.arm9i_ram, self.arm9i_size = struct.unpack_from('<4I', raw, 0x1C0)
            self.arm7i_off, _, self.arm7i_ram, self.arm7i_size = struct.unpack_from('<4I', raw, 0x1D0)
            self.digest_sec_off, self.digest_sec_size, self.digest_blk_off, self.digest_blk_size = struct.unpack_from('<4I', raw, 0x200)
            self.dsi_used_size = u32(raw, 0x210)

def banner_size(rom, off):
    return {1: 0x840, 2: 0x940, 3: 0xA40, 0x103: 0x23C0}.get(u16(rom, off), 0x840)

def parse_ovt(raw):
    out = []
    for i in range(0, len(raw) - len(raw) % 32, 32):
        oid, ram, ramsz, bss, sb, se, fid, fl = struct.unpack_from('<8I', raw, i)
        out.append(dict(id=oid, ram_addr=ram, ram_size=ramsz, bss_size=bss, sinit_start=sb,
                        sinit_end=se, file_id=fid, comp_size=fl & 0xFFFFFF, flags=fl >> 24))
    return out

def parse_fnt(fnt):
    """Return {file_id: 'path/inside/data'} and list of dir paths."""
    files, dirs = {}, []
    def walk(dir_id, prefix):
        idx = dir_id & 0xFFF
        sub_off, first_id, _ = struct.unpack_from('<IHH', fnt, idx * 8)
        p, fid = sub_off, first_id
        while True:
            ln = fnt[p]; p += 1
            if ln == 0: break
            name = fnt[p:p + (ln & 0x7F)].decode('shift_jis', 'replace'); p += ln & 0x7F
            if ln & 0x80:
                sub = u16(fnt, p); p += 2
                path = prefix + name + '/'
                dirs.append(path)
                walk(sub, path)
            else:
                files[fid] = prefix + name; fid += 1
    walk(0xF000, '')
    return files, dirs

def module_params(arm9, arm9_ram, rom, h):
    """Find ModuleParams via the 12-byte nitro footer that follows ARM9 in ROM."""
    foot = rom[h.arm9_off + h.arm9_size: h.arm9_off + h.arm9_size + 12]
    if len(foot) == 12 and u32(foot, 0) == 0xDEC00621:
        off = u32(foot, 4)
        names = ['autoload_list_start', 'autoload_list_end', 'autoload_start', 'static_bss_start',
                 'static_bss_end', 'compressed_static_end', 'sdk_version', 'nitro_code_be', 'nitro_code_le']
        vals = struct.unpack_from('<9I', arm9, off)
        return off, dict(zip(names, vals))
    return None, None

# ---------------------------------------------------------------- info / validation
def load(path):
    with open(path, 'rb') as f: return f.read()

def info(path, quiet=False):
    rom = load(path)
    h = Header(rom[:0x1000])
    report, problems = [], []
    P = report.append
    P('file          %s (%d bytes)' % (os.path.basename(path), len(rom)))
    P('title/code    %s / %s / maker %s / rev %d / unit 0x%02X%s' % (h.title, h.gamecode, h.maker, h.version, h.unitcode, ' (DSi-enhanced)' if h.is_dsi else ''))
    hc = crc16(rom[:0x15E]); lc = crc16(rom[0xC0:0x15C])
    P('header CRC    stored %04X calc %04X %s' % (h.header_crc, hc, 'OK' if hc == h.header_crc else 'BAD'))
    P('logo CRC      stored %04X calc %04X %s' % (h.logo_crc, lc, 'OK' if lc == h.logo_crc and lc == 0xCF56 else 'BAD'))
    if hc != h.header_crc: problems.append('header CRC mismatch')
    if lc != 0xCF56: problems.append('logo CRC mismatch')
    P('ARM9          rom 0x%08X size 0x%X ram 0x%08X entry 0x%08X' % (h.arm9_off, h.arm9_size, h.arm9_ram, h.arm9_entry))
    P('ARM7          rom 0x%08X size 0x%X ram 0x%08X entry 0x%08X' % (h.arm7_off, h.arm7_size, h.arm7_ram, h.arm7_entry))
    ov9 = parse_ovt(rom[h.ovt9_off:h.ovt9_off + h.ovt9_size]); ov7 = parse_ovt(rom[h.ovt7_off:h.ovt7_off + h.ovt7_size])
    P('overlays      ARM9 %d / ARM7 %d' % (len(ov9), len(ov7)))
    nfat = h.fat_size // 8
    fat = [struct.unpack_from('<II', rom, h.fat_off + i * 8) for i in range(nfat)]
    files, dirs = parse_fnt(rom[h.fnt_off:h.fnt_off + h.fnt_size])
    P('filesystem    %d FAT entries, %d named files, %d dirs' % (nfat, len(files), len(dirs)))
    bad = [i for i, (s, e) in enumerate(fat) if e < s or e > len(rom)]
    if bad: problems.append('%d FAT entries out of range' % len(bad))
    # BLZ sanity on ARM9 and overlays
    arm9 = rom[h.arm9_off:h.arm9_off + h.arm9_size]
    mpo, mp = module_params(arm9, h.arm9_ram, rom, h)
    if mp:
        P('ARM9 params   SDK 0x%08X compressed_static_end 0x%08X' % (mp['sdk_version'], mp['compressed_static_end']))
    ov_fail = 0
    for o in ov9:
        s, e = fat[o['file_id']]
        if o['flags'] & 1:
            try:
                d = blz_decompress(rom[s:e])
                if len(d) != o['ram_size']: ov_fail += 1
            except Exception:
                ov_fail += 1
    P('overlay BLZ   %d/%d decompress to their declared RAM size' % (len(ov9) - ov_fail, len(ov9)))
    if ov_fail: problems.append('%d ARM9 overlays fail to decompress' % ov_fail)
    # NARC structural check
    narcs = narc_bad = 0
    for fid in files:
        s, e = fat[fid]
        if rom[s:s + 4] == b'NARC':
            narcs += 1
            try: narc_unpack(rom[s:e])
            except Exception: narc_bad += 1
    P('NARC archives %d parsed, %d malformed' % (narcs, narc_bad))
    if narc_bad: problems.append('%d malformed NARCs' % narc_bad)
    used = h.dsi_used_size if h.is_dsi else h.used_size
    pad = rom[used:]
    nonpad = len(pad) - pad.count(0xFF)
    P('used size     0x%X (%.1f MB); trailing pad non-0xFF bytes: %d' % (used, used / 2**20, nonpad))
    if h.is_dsi:
        P('ARM9i/ARM7i   0x%X+0x%X / 0x%X+0x%X' % (h.arm9i_off, h.arm9i_size, h.arm7i_off, h.arm7i_size))
    P('CRC32 (file)  %08X' % (zlib.crc32(rom) & 0xFFFFFFFF))
    P('SHA1 (file)   %s' % hashlib.sha1(rom).hexdigest())
    P('SHA1 (used)   %s' % hashlib.sha1(rom[:used]).hexdigest())
    P('RESULT        ' + ('structurally OK' if not problems else 'PROBLEMS: ' + '; '.join(problems)))
    if not quiet: print('\n'.join(report))
    return report, problems

# ---------------------------------------------------------------- NARC
def narc_unpack(data):
    if data[:4] != b'NARC': raise ValueError('not NARC')
    hdr_size = u16(data, 0x0C)
    p = hdr_size
    blocks = {}
    for _ in range(u16(data, 0x0E)):
        magic = data[p:p + 4]; size = u32(data, p + 4)
        blocks[magic] = (p, size); p += size
    fp, _ = blocks[b'BTAF']; n = u16(data, fp + 8)
    entries = [struct.unpack_from('<II', data, fp + 12 + i * 8) for i in range(n)]
    gp, _ = blocks[b'GMIF']; base = gp + 8
    names = {}
    if b'BTNF' in blocks:
        np_, nsz = blocks[b'BTNF']
        fnt = data[np_ + 8: np_ + nsz]
        try: names, _ = parse_fnt(fnt)
        except Exception: names = {}
    out = []
    for i, (s, e) in enumerate(entries):
        if e < s or base + e > len(data): raise ValueError('bad NARC entry')
        out.append((names.get(i), data[base + s: base + e]))
    return out

def guess_ext(b):
    m = b[:4]
    for sig, ext in ((b'RGCN', '.NCGR'), (b'RLCN', '.NCLR'), (b'RECN', '.NCER'), (b'RNAN', '.NANR'),
                     (b'RCSN', '.NSCR'), (b'BMD0', '.nsbmd'), (b'BTX0', '.nsbtx'), (b'BCA0', '.nsbca'),
                     (b'BTA0', '.nsbta'), (b'BTP0', '.nsbtp'), (b'BMA0', '.nsbma'), (b'SDAT', '.sdat'),
                     (b'NARC', '.narc'), (b'BCLR', '.bclr'), (b'SPA1', '.spa')):
        if m == sig: return ext
    if b[:1] in (b'\x10', b'\x11') and len(b) > 4: return '.lz'
    return '.bin'

# ---------------------------------------------------------------- extract / rebuild
def wfile(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f: f.write(data)

def extract(path, out, unpack_narcs=False):
    rom = load(path)
    h = Header(rom[:0x1000])
    regions = []  # (off, size, relpath)
    def region(off, size, rel, data=None):
        if size == 0: return
        wfile(os.path.join(out, rel), rom[off:off + size] if data is None else data)
        regions.append((off, size, rel))

    hdr_end = 0x4000 if h.arm9_off >= 0x4000 else h.header_size
    region(0, hdr_end, 'rom/header.bin')
    arm9 = rom[h.arm9_off:h.arm9_off + h.arm9_size]
    region(h.arm9_off, h.arm9_size, 'rom/arm9.bin')
    mpo, mp = module_params(arm9, h.arm9_ram, rom, h)
    meta = dict(title=h.title, gamecode=h.gamecode, revision=h.version, size=len(rom),
                sha1=hashlib.sha1(rom).hexdigest(), arm9=dict(ram=h.arm9_ram, entry=h.arm9_entry,
                module_params_offset=mpo, module_params=mp))
    if mp and mp['compressed_static_end']:
        # region from compressed_static_end-relative is BLZ; everything before it raw
        cend = mp['compressed_static_end'] - h.arm9_ram
        # BLZ data is the tail of arm9[:cend]; the leading part stays as-is.
        dec = blz_decompress(arm9[:cend]) + arm9[cend:]
        wfile(os.path.join(out, 'decomp/arm9.decomp.bin'), dec)
    else:
        wfile(os.path.join(out, 'decomp/arm9.decomp.bin'), arm9)
    region(h.arm9_off + h.arm9_size, 12, 'rom/arm9_footer.bin') if rom[h.arm9_off + h.arm9_size:h.arm9_off + h.arm9_size + 4] == b'\x21\x06\xC0\xDE' else None
    region(h.arm7_off, h.arm7_size, 'rom/arm7.bin')
    region(h.ovt9_off, h.ovt9_size, 'rom/y9.bin')
    region(h.ovt7_off, h.ovt7_size, 'rom/y7.bin')
    region(h.fnt_off, h.fnt_size, 'rom/fnt.bin')
    region(h.fat_off, h.fat_size, 'rom/fat.bin')
    if h.banner_off: region(h.banner_off, banner_size(rom, h.banner_off), 'rom/banner.bin')
    if h.is_dsi:
        if h.digest_sec_size: region(h.digest_sec_off, h.digest_sec_size, 'rom/dsi_digest_sector.bin')
        if h.digest_blk_size: region(h.digest_blk_off, h.digest_blk_size, 'rom/dsi_digest_block.bin')
        region(h.arm9i_off, h.arm9i_size, 'rom/arm9i.bin')
        region(h.arm7i_off, h.arm7i_size, 'rom/arm7i.bin')

    fat = [struct.unpack_from('<II', rom, h.fat_off + i * 8) for i in range(h.fat_size // 8)]
    files, _ = parse_fnt(rom[h.fnt_off:h.fnt_off + h.fnt_size])
    ov9 = parse_ovt(rom[h.ovt9_off:h.ovt9_off + h.ovt9_size]); ov7 = parse_ovt(rom[h.ovt7_off:h.ovt7_off + h.ovt7_size])
    used_ids = set()
    for arm, ovs in (('arm9', ov9), ('arm7', ov7)):
        for o in ovs:
            s, e = fat[o['file_id']]
            rel = 'rom/%s_overlays/overlay_%04d.bin' % (arm, o['id'])
            region(s, e - s, rel); used_ids.add(o['file_id'])
            d = rom[s:e]
            if o['flags'] & 1:
                try: d = blz_decompress(d)
                except Exception as ex:
                    print('WARNING: %s overlay %d failed to decompress (%s); raw copy only' % (arm, o['id'], ex))
                    meta.setdefault('damaged_overlays', []).append(o['id']); continue
            wfile(os.path.join(out, 'decomp/%s_overlays/overlay_%04d.decomp.bin' % (arm, o['id'])), d)
    meta['overlays9'] = ov9; meta['overlays7'] = ov7
    for fid, (s, e) in enumerate(fat):
        if fid in used_ids: continue
        rel = 'data/' + files[fid] if fid in files else 'unnamed/file_%04d.bin' % fid
        if e > s: region(s, e - s, rel)
        else: wfile(os.path.join(out, rel), b'')
    meta['files'] = {str(k): v for k, v in files.items()}

    # gaps -> fill records or blobs so the layout covers every byte
    regions.sort()
    layout, pos, gi = [], 0, 0
    for off, size, rel in regions:
        if off < pos:
            # overlapping/duplicate region: record as alias, it is already covered
            layout.append(dict(off=off, size=size, path=rel, alias=True)); continue
        if off > pos:
            gap = rom[pos:off]
            if gap.count(gap[0]) == len(gap): layout.append(dict(off=pos, size=len(gap), fill=gap[0]))
            else:
                grel = 'rom/gaps/gap_%03d_%08X.bin' % (gi, pos); gi += 1
                wfile(os.path.join(out, grel), gap); layout.append(dict(off=pos, size=len(gap), path=grel))
        layout.append(dict(off=off, size=size, path=rel)); pos = off + size
    used = h.dsi_used_size if h.is_dsi else h.used_size
    if pos < used:
        gap = rom[pos:used]
        if gap.count(gap[0]) == len(gap): layout.append(dict(off=pos, size=len(gap), fill=gap[0]))
        else:
            grel = 'rom/gaps/gap_%03d_%08X.bin' % (gi, pos)
            wfile(os.path.join(out, grel), gap); layout.append(dict(off=pos, size=len(gap), path=grel))
        pos = used
    if pos < len(rom):
        tail = rom[pos:]
        if tail.count(0xFF) == len(tail):
            layout.append(dict(off=pos, size=len(tail), fill=0xFF))
        else:
            # Overdump garbage past the used size (seen in the B2 [b] dump): not stored.
            # Matching target becomes the trimmed image (bytes 0..used_size).
            meta['trimmed'] = True
            print('note: %d bytes of non-padding data after used size dropped; target is trimmed ROM' % len(tail))
    meta['target_sha1'] = hashlib.sha1(rom[:pos if meta.get('trimmed') else len(rom)]).hexdigest()
    meta['layout'] = layout
    with open(os.path.join(out, 'layout.json'), 'w', encoding='utf-8') as f:
        json.dump(meta, f, indent=1, ensure_ascii=False)

    if unpack_narcs:
        n = 0
        for fid, rel in files.items():
            s, e = fat[fid]
            if rom[s:s + 4] != b'NARC': continue
            try: members = narc_unpack(rom[s:e])
            except Exception: continue
            base = os.path.join(out, 'narc_unpacked', rel)
            for i, (name, blob) in enumerate(members):
                wfile(os.path.join(base, name if name else '%04d%s' % (i, guess_ext(blob))), blob)
            n += 1
        print('unpacked %d NARCs' % n)
    print('extracted %s -> %s (%d regions)' % (os.path.basename(path), out, len(layout)))

def rebuild(tree, outpath):
    with open(os.path.join(tree, 'layout.json'), encoding='utf-8') as f: meta = json.load(f)
    with open(outpath, 'wb') as o:
        for r in meta['layout']:
            if r.get('alias'): continue
            if o.tell() != r['off']: raise SystemExit('layout discontinuity at 0x%X' % r['off'])
            if 'fill' in r:
                left = r['size']
                while left:
                    n = min(left, 1 << 20); o.write(bytes([r['fill']]) * n); left -= n
            else:
                with open(os.path.join(tree, r['path']), 'rb') as f: d = f.read()
                if len(d) != r['size']:
                    raise SystemExit('%s changed size (%d != %d); relayout not implemented yet' % (r['path'], len(d), r['size']))
                o.write(d)
    return meta

def verify(rom, tree):
    fd, tmp = tempfile.mkstemp(suffix='.nds'); os.close(fd)
    try:
        meta = rebuild(tree, tmp)
        a = sha1_file(rom, os.path.getsize(tmp) if meta.get('trimmed') else None)
        b = sha1_file(tmp)
        if meta.get('trimmed'): print('(comparing trimmed image, %d bytes)' % os.path.getsize(tmp))
        print('original %s\nrebuilt  %s\n%s' % (a, b, 'MATCH' if a == b else 'MISMATCH'))
        return a == b
    finally:
        os.remove(tmp)

if __name__ == '__main__':
    a = sys.argv[1:]
    if not a: print(__doc__); sys.exit(1)
    if a[0] == 'info': sys.exit(1 if info(a[1])[1] else 0)
    elif a[0] == 'extract': extract(a[1], a[2], unpack_narcs='--narcs' in a)
    elif a[0] == 'rebuild': rebuild(a[1], a[2])
    elif a[0] == 'verify': sys.exit(0 if verify(a[1], a[2]) else 1)
    else: print(__doc__); sys.exit(1)
