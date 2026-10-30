/* Machine side of the recompiled ARM9: memory map, I/O registers, CP15, PSR and BIOS calls. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

uint8_t *rc_page[1 << 20];

static uint8_t main_ram[4 << 20];
static uint8_t shared_wram[32 << 10];
static uint8_t itcm[32 << 10];
static uint8_t dtcm[16 << 10];
static uint8_t palette[2 << 10];
static uint8_t oam[2 << 10];
static uint8_t *vram;                 /* 16 MB flat for now; VRAMCNT bank mapping comes later */
static uint8_t bios[32 << 10];
static uint8_t io[64 << 10];          /* backing store for 0x04000000-0x0400ffff */

static uint32_t dtcm_base = 0x027c0000, itcm_size = 32 << 20;
static uint32_t cp15_ctl = 0x00052078, cp15_regs[16][16][8];

FILE *rc_log;

static void logf_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(rc_log ? rc_log : stderr, fmt, ap);
    va_end(ap);
}

static void fatal(Cpu *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FATAL: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n  r0-r3  %08x %08x %08x %08x\n  r4-r7  %08x %08x %08x %08x\n  r8-r11 %08x %08x %08x %08x\n"
            "  r12 %08x sp %08x lr %08x  cpsr %08x\n", c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[5], c->r[6],
            c->r[7], c->r[8], c->r[9], c->r[10], c->r[11], c->r[12], c->r[13], c->r[14], rc_get_cpsr(c));
    if (rc_log) fflush(rc_log);
    exit(1);
}

/* ------------------------------------------------------------------ memory map */

static void map(uint32_t start, uint32_t end, uint8_t *mem, uint32_t size) {
    for (uint32_t a = start; a < end && a >= start; a += 0x1000)
        rc_page[a >> 12] = mem ? mem + ((a - start) & (size - 1)) : NULL;
}

/* Rebuilds the page table: regions first, then the TCMs on top of whatever they cover */
static void remap(void) {
    memset(rc_page, 0, sizeof rc_page);
    map(0x02000000, 0x03000000, main_ram, sizeof main_ram);
    map(0x03000000, 0x04000000, shared_wram, sizeof shared_wram);
    map(0x05000000, 0x06000000, palette, sizeof palette);
    map(0x06000000, 0x07000000, vram, 16 << 20);
    map(0x07000000, 0x08000000, oam, sizeof oam);
    map(0xffff0000, 0xffffffff, bios, sizeof bios);
    rc_page[0xfffff] = bios + 0x7000;
    map(0x00000000, itcm_size, itcm, sizeof itcm);
    map(dtcm_base, dtcm_base + sizeof dtcm, dtcm, sizeof dtcm);
}

/* ------------------------------------------------------------------ I/O */

uint16_t arm7_ipcsync_read(uint16_t stored);
void arm7_ipcsync_write(uint16_t v);
uint16_t arm7_fifocnt_read(void);
void arm7_fifocnt_write(uint16_t v);
void arm7_fifo_write(uint32_t v);
uint32_t arm7_fifo_read(void);
void card_open(const char *path);
uint32_t card_chip_id(void);
void card_cmd_write(uint32_t off, uint8_t v);
void card_romctrl_write(uint32_t v);
uint32_t card_romctrl_read(void);
int card_data_ready(void);
uint32_t card_data_read(void);

static uint16_t vcount;
static uint64_t frames;

static uint16_t io16(uint32_t off) { uint16_t v; memcpy(&v, io + off, 2); return v; }
static uint32_t io32(uint32_t off) { uint32_t v; memcpy(&v, io + off, 4); return v; }
static void set_io32(uint32_t off, uint32_t v) { memcpy(io + off, &v, 4); }

/* Logs the first access to each I/O register, which shows what the game expects from the hardware */
static uint8_t io_seen[64 << 10];
static void note_io(uint32_t a, int write, uint32_t v) {
    uint32_t off = a & 0xffff;
    if (a >= 0x04010000 || io_seen[off] & (1 << write)) return;
    io_seen[off] |= 1 << write;
    logf_("io %s %08x = %08x (first)\n", write ? "write" : "read ", a, v);
}

static void advance_scanline(void) {
    vcount = (vcount + 1) % 263;
    if (vcount == 0) frames++;
    uint16_t stat = io16(4) & ~7;
    if (vcount >= 192 && vcount < 262) stat |= 1;
    if (vcount == (((io16(4) >> 8) | ((io16(4) & 0x80) << 1)))) stat |= 4;
    memcpy(io + 4, &stat, 2);
    memcpy(io + 6, &vcount, 2);
}

static void hw_divide(void) {
    uint16_t cnt = io16(0x280);
    int64_t num, den;
    switch (cnt & 3) {
    case 0: num = (int32_t)io32(0x290); den = (int32_t)io32(0x298); break;
    case 1: num = (int64_t)((uint64_t)io32(0x290) | (uint64_t)io32(0x294) << 32); den = (int32_t)io32(0x298); break;
    default: num = (int64_t)((uint64_t)io32(0x290) | (uint64_t)io32(0x294) << 32);
             den = (int64_t)((uint64_t)io32(0x298) | (uint64_t)io32(0x29c) << 32); break;
    }
    cnt = (cnt & ~0x4000) | (den == 0 ? 0x4000 : 0);
    memcpy(io + 0x280, &cnt, 2);
    int64_t q, rem;
    if (den == 0) { q = num < 0 ? 1 : -1; rem = num; }
    else if (num == INT64_MIN && den == -1) { q = num; rem = 0; }
    else { q = num / den; rem = num % den; }
    set_io32(0x2a0, (uint32_t)q); set_io32(0x2a4, (uint32_t)((uint64_t)q >> 32));
    set_io32(0x2a8, (uint32_t)rem); set_io32(0x2ac, (uint32_t)((uint64_t)rem >> 32));
}

static void hw_sqrt(void) {
    uint64_t v = io32(0x2b8);
    if (io16(0x2b0) & 1) v |= (uint64_t)io32(0x2bc) << 32;
    uint64_t r = 0, bit = 1ull << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
        bit >>= 2;
    }
    set_io32(0x2b4, (uint32_t)r);
}

static void dma_start(int ch) {
    uint32_t base = 0xb0 + ch * 12;
    uint32_t src = io32(base), dst = io32(base + 4), cnt = io32(base + 8);
    if (!(cnt & 0x80000000u)) return;
    uint32_t timing = (cnt >> 27) & 7;
    if (timing == 5) return;                /* card: runs when a card transfer starts, see rc_card_dma */
    if (timing != 0) {
        logf_("dma%d timing %u not emulated yet (src %08x dst %08x cnt %08x)\n", ch, timing, src, dst, cnt);
        return;
    }
    uint32_t n = cnt & 0x1fffff, word = cnt & (1 << 26) ? 4 : 2;
    int sd = (cnt >> 23) & 3, dd = (cnt >> 21) & 3;
    if (!n) n = 0x200000;
    for (uint32_t i = 0; i < n; i++) {
        if (word == 4) wr32(dst, rd32(src)); else wr16(dst, rd16(src));
        src += sd == 0 ? word : sd == 1 ? -(int32_t)word : 0;
        dst += dd == 0 || dd == 3 ? word : dd == 1 ? -(int32_t)word : 0;
    }
    set_io32(base + 8, cnt & ~0x80000000u);
}

/* DMA channels set to card timing move the card's data as soon as a transfer starts */
void rc_card_dma(void) {
    for (int ch = 0; ch < 4; ch++) {
        uint32_t base = 0xb0 + ch * 12, cnt = io32(base + 8);
        if (!(cnt & 0x80000000u) || ((cnt >> 27) & 7) != 5) continue;
        uint32_t dst = io32(base + 4);
        int dd = (cnt >> 21) & 3;
        while (card_data_ready()) {
            wr32(dst, card_data_read());
            dst += dd == 0 || dd == 3 ? 4 : dd == 1 ? (uint32_t)-4 : 0;
        }
        set_io32(base + 4, dst);
        if (!(cnt & (1u << 25))) set_io32(base + 8, cnt & ~0x80000000u);
    }
}

void rc_raise_irq(uint32_t bits) { set_io32(0x214, io32(0x214) | bits); }

uint32_t rc_io_rd32(uint32_t a) {
    uint32_t v;
    if (a >= 0x04000000 && a < 0x04010000) {
        uint32_t off = a & 0xffff;
        if (off == 4) advance_scanline();
        v = io32(off);
        if (off == 0x130) v = (v & 0xffff0000) | 0x03ff;            /* KEYINPUT: nothing pressed */
        if (off == 0x180) v = (v & 0xffff0000) | arm7_ipcsync_read((uint16_t)v);
        if (off == 0x184) v = arm7_fifocnt_read();
        if (off == 0x1a4) v = card_romctrl_read();
    } else if (a == 0x04100000) {
        v = arm7_fifo_read();
    } else if (a == 0x04100010) {
        v = card_data_read();
    } else if (a >= 0x08000000 && a < 0x0a000000) {
        v = 0xffffffff;                                            /* empty GBA slot */
    } else {
        v = 0;
    }
    note_io(a, 0, v);
    return v;
}
uint16_t rc_io_rd16(uint32_t a) {
    uint16_t v;
    if (a >= 0x04000000 && a < 0x04010000) {
        uint32_t off = a & 0xffff;
        if (off == 4 || off == 6) advance_scanline();
        v = io16(off);
        if (off == 0x130) v = 0x03ff;
        if (off == 0x180) v = arm7_ipcsync_read(v);
        if (off == 0x184) v = arm7_fifocnt_read();
    } else if (a == 0x04100000) {
        v = (uint16_t)arm7_fifo_read();
    } else if (a >= 0x08000000 && a < 0x0a000000) {
        v = 0xffff;
    } else {
        v = 0;
    }
    note_io(a, 0, v);
    return v;
}
uint8_t rc_io_rd8(uint32_t a) {
    return (uint8_t)(rc_io_rd16(a & ~1u) >> ((a & 1) * 8));
}

static void io_written(uint32_t off) {
    if (off >= 0x280 && off < 0x2a0) hw_divide();
    if (off >= 0x2b0 && off < 0x2c0) hw_sqrt();
    if (off >= 0xb8 && off < 0xe0 && ((off - 0xb0) % 12) >= 8) dma_start((off - 0xb0) / 12);
    if (off == 0x180) arm7_ipcsync_write(io16(0x180));
    if (off == 0x184) arm7_fifocnt_write(io16(0x184));
    if (off == 0x188) arm7_fifo_write(io32(0x188));
    if (off >= 0x1a8 && off < 0x1b0) for (int i = 0; i < 8; i++) card_cmd_write(i, io[0x1a8 + i]);
    if (off == 0x1a4) card_romctrl_write(io32(0x1a4));
}
void rc_io_wr32(uint32_t a, uint32_t v) {
    note_io(a, 1, v);
    if (a < 0x04000000 || a >= 0x04010000) return;
    uint32_t off = a & 0xffff;
    if (off == 0x214) { set_io32(0x214, io32(0x214) & ~v); return; }
    set_io32(off, v);
    io_written(off);
}
void rc_io_wr16(uint32_t a, uint16_t v) {
    note_io(a, 1, v);
    if (a < 0x04000000 || a >= 0x04010000) return;
    uint32_t off = a & 0xffff;
    if (off == 0x214 || off == 0x216) { uint16_t f = io16(off) & ~v; memcpy(io + off, &f, 2); return; }
    memcpy(io + off, &v, 2);
    io_written(off);
}
void rc_io_wr8(uint32_t a, uint8_t v) {
    note_io(a, 1, v);
    if (a < 0x04000000 || a >= 0x04010000) return;
    io[a & 0xffff] = v;
    io_written(a & 0xffff);
}

/* ------------------------------------------------------------------ dispatch */

/* Overlays currently in memory, indexed by overlay id; set when the game loads one (not hooked yet) */
static uint8_t overlay_loaded[512];

static RcFn lookup(const RcModule *m, uint32_t addr) {
    uint32_t lo = 0, hi = m->count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (m->funcs[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    return lo < m->count && m->funcs[lo].addr == addr ? m->funcs[lo].fn : NULL;
}

/* A code copy applies while guest memory at its run address holds the bytes it was recompiled from */
static int copy_present(const RcModule *m) {
    for (uint32_t i = 0; i < m->end - m->start; i += 4)
        if (rd32(m->start + i) != rd32(m->copy_src + i)) return 0;
    return 1;
}

void rc_call(Cpu *c, uint32_t addr) {
    uint32_t a = addr & ~1u;
    for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < rc_module_count; i++) {
        const RcModule *m = &rc_modules[i];
        if ((m->copy_src != 0) != (pass == 0) || a < m->start || a >= m->end) continue;
        if (m->copy_src && !copy_present(m)) continue;
        if (m->overlay >= 0 && !overlay_loaded[m->overlay]) continue;
        RcFn fn = lookup(m, a);
        if (fn) { fn(c); return; }
    }
    fatal(c, "call to %08x: no recompiled function there (lr %08x)", addr, c->r[14]);
}

void arm7_tick(void);

static uint32_t last_tick_addr, same_tick;
void rc_tick(Cpu *c, uint32_t addr) {
    c->budget = 4096;
    for (int i = 0; i < 64; i++) advance_scanline();
    arm7_tick();
    static uint64_t ticks;
    if (++ticks % 20000 == 0)
        fprintf(stderr, "tick %llu: frame %llu, loop at %08x, lr %08x\n", (unsigned long long)ticks,
                (unsigned long long)frames, addr, c->r[14]);
    same_tick = addr == last_tick_addr ? same_tick + 1 : 0;
    last_tick_addr = addr;
    if (same_tick == 20000)
        fatal(c, "stuck in the loop at %08x (frame %llu, vcount %u)", addr, (unsigned long long)frames, vcount);
}

void rc_bad_jump(Cpu *c, uint32_t at, uint32_t target) { fatal(c, "computed jump at %08x to %08x has no case", at, target); }
void rc_unimpl(Cpu *c, uint32_t at, const char *what) { fatal(c, "unimplemented instruction at %08x: %s", at, what); }
void rc_trap(Cpu *c, uint32_t at) { fatal(c, "breakpoint/undefined instruction at %08x", at); }

/* ------------------------------------------------------------------ PSR and modes */

static int mode_slot(uint32_t mode) {
    switch (mode & 0x1f) {
    case 0x11: return 1;  /* fiq */
    case 0x12: return 2;  /* irq */
    case 0x13: return 3;  /* svc */
    case 0x17: return 4;  /* abt */
    case 0x1b: return 5;  /* und */
    default: return 0;    /* usr, sys */
    }
}

static void set_ctl(Cpu *c, uint32_t ctl) {
    int from = mode_slot(c->ctl), to = mode_slot(ctl);
    if (from != to) {
        c->bank_sp[from] = c->r[13]; c->bank_lr[from] = c->r[14]; c->bank_spsr[from] = c->spsr;
        c->r[13] = c->bank_sp[to]; c->r[14] = c->bank_lr[to]; c->spsr = c->bank_spsr[to];
    }
    c->ctl = ctl & 0xff;
}

uint32_t rc_get_cpsr(Cpu *c) {
    return c->n << 31 | c->z << 30 | c->c << 29 | c->v << 28 | c->q << 27 | c->ctl;
}
uint32_t rc_get_spsr(Cpu *c) { return c->spsr; }

void rc_msr(Cpu *c, int spsr, uint32_t mask, uint32_t v) {
    if (spsr) { c->spsr = (c->spsr & ~mask) | (v & mask); return; }
    uint32_t cpsr = (rc_get_cpsr(c) & ~mask) | (v & mask);
    c->n = cpsr >> 31; c->z = (cpsr >> 30) & 1; c->c = (cpsr >> 29) & 1; c->v = (cpsr >> 28) & 1; c->q = (cpsr >> 27) & 1;
    if (mask & 0xff) set_ctl(c, cpsr & 0xff);
}

void rc_exception_return(Cpu *c) {
    uint32_t spsr = c->spsr;
    rc_msr(c, 0, 0xffffffff, spsr);
}

/* ------------------------------------------------------------------ CP15 */

void rc_mcr(Cpu *c, int cp, int opc1, int crn, int crm, int opc2, uint32_t v) {
    (void)c;
    if (cp != 15) { logf_("mcr p%d ignored\n", cp); return; }
    cp15_regs[crn & 15][crm & 15][opc2 & 7] = v;
    if (crn == 1 && crm == 0) cp15_ctl = v;
    if (crn == 9 && crm == 1 && opc2 == 0) {
        dtcm_base = v & 0xfffff000;
        logf_("cp15: DTCM at %08x (%08x)\n", dtcm_base, v);
        remap();
    }
    if (crn == 9 && crm == 1 && opc2 == 1) {
        itcm_size = 512u << ((v >> 1) & 0x1f);
        logf_("cp15: ITCM size %08x (%08x)\n", itcm_size, v);
        remap();
    }
    (void)opc1;
}

uint32_t rc_mrc(Cpu *c, int cp, int opc1, int crn, int crm, int opc2) {
    (void)c; (void)opc1;
    if (cp != 15) return 0;
    if (crn == 0 && crm == 0) return opc2 == 1 ? 0x0f0d2112 : opc2 == 2 ? 0x00140180 : 0x41059461;
    if (crn == 1 && crm == 0) return cp15_ctl;
    return cp15_regs[crn & 15][crm & 15][opc2 & 7];
}

/* ------------------------------------------------------------------ BIOS */

static void lz77_uncomp(uint32_t src, uint32_t dst, int vram16) {
    uint32_t hdr = rd32(src), size = hdr >> 8, out = 0;
    src += 4;
    uint16_t pend = 0;
    while (out < size) {
        uint8_t flags = rd8(src++);
        for (int i = 0; i < 8 && out < size; i++, flags <<= 1) {
            if (flags & 0x80) {
                uint16_t b = rd8(src) << 8 | rd8(src + 1);
                src += 2;
                uint32_t len = (b >> 12) + 3, disp = (b & 0xfff) + 1;
                for (uint32_t j = 0; j < len && out < size; j++, out++) {
                    uint8_t v = rd8(dst + out - disp);
                    if (vram16) { pend = out & 1 ? pend | v << 8 : v; if (out & 1) wr16(dst + out - 1, pend); }
                    else wr8(dst + out, v);
                }
            } else {
                uint8_t v = rd8(src++);
                if (vram16) { pend = out & 1 ? pend | v << 8 : v; if (out & 1) wr16(dst + out - 1, pend); }
                else wr8(dst + out, v);
                out++;
            }
        }
    }
}

static void rl_uncomp(uint32_t src, uint32_t dst) {
    uint32_t size = rd32(src) >> 8, out = 0;
    src += 4;
    while (out < size) {
        uint8_t f = rd8(src++);
        if (f & 0x80) {
            uint8_t v = rd8(src++);
            for (int j = 0; j < (f & 0x7f) + 3 && out < size; j++) wr8(dst + out++, v);
        } else {
            for (int j = 0; j < (f & 0x7f) + 1 && out < size; j++) wr8(dst + out++, rd8(src++));
        }
    }
}

void rc_swi(Cpu *c, uint32_t n, uint32_t at) {
    switch (n) {
    case 0x03: break;                                           /* WaitByLoop */
    case 0x04: case 0x05: case 0x06:                            /* IntrWait, VBlankIntrWait, Halt */
        for (int i = 0; i < 263; i++) advance_scanline();
        break;
    case 0x09: {                                                /* Div */
        int32_t num = (int32_t)c->r[0], den = (int32_t)c->r[1];
        if (den == 0) fatal(c, "SWI Div by zero at %08x", at);
        c->r[0] = (uint32_t)(num / den); c->r[1] = (uint32_t)(num % den);
        c->r[3] = (uint32_t)abs(num / den);
        break;
    }
    case 0x0b: case 0x0c: {                                     /* CpuSet, CpuFastSet */
        uint32_t src = c->r[0], dst = c->r[1], cnt = c->r[2], len = cnt & 0x1fffff;
        int fill = (cnt >> 24) & 1, word = n == 0x0c || (cnt >> 26) & 1;
        for (uint32_t i = 0; i < len; i++) {
            if (word) wr32(dst + i * 4, rd32(fill ? src : src + i * 4));
            else wr16(dst + i * 2, rd16(fill ? src : src + i * 2));
        }
        break;
    }
    case 0x0d: {                                                /* Sqrt */
        uint32_t v = c->r[0], r = 0;
        while ((uint64_t)(r + 1) * (r + 1) <= v) r++;
        c->r[0] = r;
        break;
    }
    case 0x0e: {                                                /* GetCRC16: CRC-16 with reflected 0x8005 */
        uint32_t crc = c->r[0] & 0xffff;
        for (uint32_t i = 0; i < c->r[2]; i++) {
            crc ^= rd8(c->r[1] + i);
            for (int j = 0; j < 8; j++) crc = crc & 1 ? (crc >> 1) ^ 0xa001 : crc >> 1;
        }
        c->r[0] = crc;
        break;
    }
    case 0x11: lz77_uncomp(c->r[0], c->r[1], 0); break;        /* LZ77UnCompReadNormalWrite8bit */
    case 0x12: lz77_uncomp(c->r[0], c->r[1], 1); break;        /* LZ77UnCompReadNormalWrite16bit */
    case 0x14: case 0x15: rl_uncomp(c->r[0], c->r[1]); break;  /* RLUnComp */
    default:
        logf_("swi %02x at %08x not implemented\n", n, at);
    }
}

/* ------------------------------------------------------------------ boot */

static void boot(Cpu *c, const char *rom_path) {
    FILE *f = fopen(rom_path, "rb");
    if (!f) { perror(rom_path); exit(1); }
    static uint8_t header[0x1000];
    fread(header, 1, sizeof header, f);
    uint32_t arm9_off, arm9_entry, arm9_ram, arm9_size;
    memcpy(&arm9_off, header + 0x20, 4); memcpy(&arm9_entry, header + 0x24, 4);
    memcpy(&arm9_ram, header + 0x28, 4); memcpy(&arm9_size, header + 0x2c, 4);

    vram = calloc(16 << 20, 1);
    remap();

    /* What the DS firmware leaves behind: the ARM9 binary as stored in the ROM (the game decompresses it itself),
     * and the cartridge header at the top of main RAM */
    uint8_t *buf = malloc(arm9_size);
    fseek(f, arm9_off, SEEK_SET);
    fread(buf, 1, arm9_size, f);
    for (uint32_t i = 0; i < arm9_size; i++) wr8(arm9_ram + i, buf[i]);
    free(buf);
    for (uint32_t i = 0; i < 0x170; i++) wr8(0x027ffe00 + i, header[i]);
    fclose(f);
    card_open(rom_path);
    wr32(0x027ff800, card_chip_id());       /* card ID as read at boot, which the SDK compares against */
    wr32(0x027ffc00, card_chip_id());
    wr16(0x027ffc40, 1);                    /* booted from a card */

    memset(c, 0, sizeof *c);
    c->ctl = 0xdf;            /* system mode, IRQ and FIQ disabled */
    c->r[13] = 0x03002f7c;
    c->r[15] = arm9_entry;
    c->budget = 4096;
    logf_("boot: ARM9 %u bytes at %08x, entry %08x\n", arm9_size, arm9_ram, arm9_entry);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <rom.nds>\n", argv[0]);
        return 2;
    }
    rc_log = fopen("b2w2.log", "w");
    if (rc_log) setvbuf(rc_log, NULL, _IONBF, 0);  /* MSVC has no line buffering */
    static Cpu cpu;
    boot(&cpu, argv[1]);
    rc_call(&cpu, cpu.r[15]);
    fprintf(stderr, "entry function returned (lr %08x)\n", cpu.r[14]);
    if (rc_log) fclose(rc_log);
    return 0;
}
