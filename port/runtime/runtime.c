/* Machine side of the recompiled ARM9: memory map, I/O registers, CP15, PSR and BIOS calls. */
#include "internal.h"
#include <stdlib.h>
#include <stdarg.h>
#include <stdlib.h>

uint8_t *rc_page[1 << 20];
Cpu rc_cpu;

static uint8_t main_ram[4 << 20];
static uint32_t watch_lo, watch_hi;          /* debugging: B2W2_WATCH (below) */
static uint8_t shared_wram[32 << 10];
static uint8_t itcm[32 << 10];
static uint8_t dtcm[16 << 10];
uint8_t rc_palette[2 << 10], rc_oam[2 << 10];
static uint8_t bios[32 << 10];
uint8_t rc_io[64 << 10];

/* VRAM banks A-I */
static const uint32_t bank_size[9] = {128 << 10, 128 << 10, 128 << 10, 128 << 10, 64 << 10, 16 << 10, 16 << 10,
                                      32 << 10, 16 << 10};
static const uint32_t bank_lcdc[9] = {0x06800000, 0x06820000, 0x06840000, 0x06860000, 0x06880000, 0x06890000,
                                      0x06894000, 0x06898000, 0x068a0000};
static uint8_t vram_mem[656 << 10];
static uint8_t *bank_ptr[9];

uint32_t rc_dtcm_base = 0x027c0000;
static uint32_t itcm_size = 32 << 20;
static uint32_t cp15_ctl = 0x00052078, cp15_regs[16][16][8];

FILE *rc_log;

void rc_logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(rc_log ? rc_log : stderr, fmt, ap);
    va_end(ap);
}

void rc_fatal(Cpu *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FATAL: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n  r0-r3  %08x %08x %08x %08x\n  r4-r7  %08x %08x %08x %08x\n  r8-r11 %08x %08x %08x %08x\n"
            "  r12 %08x sp %08x lr %08x  cpsr %08x  frame %llu\n", c->r[0], c->r[1], c->r[2], c->r[3], c->r[4],
            c->r[5], c->r[6], c->r[7], c->r[8], c->r[9], c->r[10], c->r[11], c->r[12], c->r[13], c->r[14],
            rc_get_cpsr(c), (unsigned long long)rc_frames);
    /* the stack's top words: return addresses there tell who called */
    const char *rc_last_overlay(void);
    fprintf(stderr, "  last overlay called: %s\n", rc_last_overlay());
    fprintf(stderr, "  stack");
    for (int i = 0; i < 32; i++) fprintf(stderr, "%s%08x", i % 8 ? " " : "\n   ", rd32(c->r[13] + i * 4));
    fprintf(stderr, "\n");
    if (rc_log) {
        va_start(ap, fmt);
        fprintf(rc_log, "FATAL: ");
        vfprintf(rc_log, fmt, ap);
        va_end(ap);
        fprintf(rc_log, " (frame %llu)\n", (unsigned long long)rc_frames);
        fflush(rc_log);
    }
    exit(1);
}

/* ------------------------------------------------------------------ memory map */

static void map(uint32_t start, uint32_t end, uint8_t *mem, uint32_t size) {
    for (uint32_t a = start; a < end && a >= start; a += 0x1000)
        rc_page[a >> 12] = mem ? mem + ((a - start) & (size - 1)) : NULL;
}

/* Where each bank appears to the CPU for its VRAMCNT setting (GBATEK "DS Memory Control - VRAM") */
static void map_bank(int b) {
    uint8_t cnt = rc_io[0x240 + b + (b >= 7 ? 1 : 0)];
    if (!(cnt & 0x80)) return;
    uint32_t mst = cnt & (b <= 1 ? 3 : 7), ofs = (cnt >> 3) & 3, size = bank_size[b], at = 0;
    switch (b) {
    case 0: case 1: case 2: case 3:
        if (mst == 0) at = bank_lcdc[b];
        else if (mst == 1) at = 0x06000000 + 0x20000 * ofs;
        else if (mst == 2 && b <= 1) at = 0x06400000 + 0x20000 * (ofs & 1);
        else if (mst == 4 && b == 2) at = 0x06200000;
        else if (mst == 4 && b == 3) at = 0x06600000;
        break;
    case 4:
        if (mst == 0) at = bank_lcdc[b];
        else if (mst == 1) at = 0x06000000;
        else if (mst == 2) at = 0x06400000;
        break;
    case 5: case 6:
        if (mst == 0) at = bank_lcdc[b];
        else if (mst == 1) at = 0x06000000 + 0x4000 * (ofs & 1) + 0x10000 * (ofs >> 1);
        else if (mst == 2) at = 0x06400000 + 0x4000 * (ofs & 1) + 0x10000 * (ofs >> 1);
        break;
    case 7:
        if (mst == 0) at = bank_lcdc[b];
        else if (mst == 1) at = 0x06200000;
        break;
    case 8:
        if (mst == 0) at = bank_lcdc[b];
        else if (mst == 1) at = 0x06208000;
        else if (mst == 2) at = 0x06600000;
        break;
    }
    if (!at) return;
    /* Each engine's BG/OBJ area mirrors over its whole range */
    uint32_t region = at >= 0x06800000 ? 0x06800000 : at & 0xffe00000;
    uint32_t span = region == 0x06000000 ? 0x80000 : region == 0x06200000 ? 0x20000 :
                    region == 0x06400000 ? 0x40000 : region == 0x06600000 ? 0x20000 : 0x100000;
    for (uint32_t m = region; m < region + 0x200000 && m >= region; m += span)
        map(m + (at - region), m + (at - region) + size, bank_ptr[b], size);
}

void rc_remap_vram(void) {
    map(0x06000000, 0x07000000, NULL, 0);
    for (int b = 0; b < 9; b++) map_bank(b);
}

static void remap(void) {
    memset(rc_page, 0, sizeof rc_page);
    map(0x02000000, 0x03000000, main_ram, sizeof main_ram);
    map(0x03000000, 0x04000000, shared_wram, sizeof shared_wram);
    map(0x05000000, 0x06000000, rc_palette, sizeof rc_palette);
    map(0x07000000, 0x08000000, rc_oam, sizeof rc_oam);
    map(0xffff0000, 0xffffffff, bios, sizeof bios);
    rc_page[0xfffff] = bios + 0x7000;
    rc_remap_vram();
    map(0x00000000, itcm_size, itcm, sizeof itcm);
    map(rc_dtcm_base, rc_dtcm_base + sizeof dtcm, dtcm, sizeof dtcm);
    if (watch_hi)                                       /* debugging: B2W2_WATCH */
        for (uint32_t m = 0x02000000; m < 0x03000000; m += 0x400000) map(m + watch_lo, m + watch_hi, NULL, 0);
}

uint8_t *rc_vram_bank(int b) { return bank_ptr[b]; }

uint8_t *rc_vram_lcdc(uint32_t addr) {
    for (int b = 0; b < 9; b++)
        if (addr >= bank_lcdc[b] && addr < bank_lcdc[b] + bank_size[b]) return bank_ptr[b] + (addr - bank_lcdc[b]);
    return NULL;
}

static uint8_t bank_cnt(int b) { return rc_io[0x240 + b + (b >= 7 ? 1 : 0)]; }

uint8_t *rc_tex_slot(int slot) {
    for (int b = 0; b < 4; b++) {
        uint8_t cnt = bank_cnt(b);
        if ((cnt & 0x87) == 0x83 && ((cnt >> 3) & 3) == slot) return bank_ptr[b];
    }
    return NULL;
}

uint8_t *rc_texpal_slot(int slot) {
    uint8_t e = bank_cnt(4);
    if ((e & 0x87) == 0x83 && slot < 4) return bank_ptr[4] + slot * 0x4000;
    for (int b = 5; b <= 6; b++) {
        uint8_t cnt = bank_cnt(b), ofs = (cnt >> 3) & 3;
        if ((cnt & 0x87) == 0x83 && (ofs & 1) + (ofs >> 1) * 4 == slot) return bank_ptr[b];
    }
    return NULL;
}

uint8_t *rc_bg_extpal(int engine, int slot) {
    if (engine == 0) {
        uint8_t e = bank_cnt(4);
        if ((e & 0x87) == 0x84) return bank_ptr[4] + slot * 0x2000;
        for (int b = 5; b <= 6; b++) {
            uint8_t cnt = bank_cnt(b), ofs = (cnt >> 3) & 3;
            if ((cnt & 0x87) == 0x84 && slot / 2 == (ofs & 1)) return bank_ptr[b] + (slot & 1) * 0x2000;
        }
    } else if ((bank_cnt(7) & 0x83) == 0x82) {
        return bank_ptr[7] + slot * 0x2000;
    }
    return NULL;
}

uint8_t *rc_obj_extpal(int engine) {
    if (engine == 0) {
        for (int b = 5; b <= 6; b++)
            if ((bank_cnt(b) & 0x87) == 0x85) return bank_ptr[b];
    } else if ((bank_cnt(8) & 0x83) == 0x83) {
        return bank_ptr[8];
    }
    return NULL;
}

/* ------------------------------------------------------------------ I/O */

/* Logs the first access to each I/O register, which shows what the game expects from the hardware */
static uint8_t io_seen[64 << 10];
static void note_io(uint32_t a, int write, uint32_t v) {
    uint32_t off = a & 0xffff;
    if (a >= 0x04010000 || io_seen[off] & (1 << write)) return;
    io_seen[off] |= 1 << write;
    rc_logf("io %s %08x = %08x (first)\n", write ? "write" : "read ", a, v);
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
    set_io16(0x280, cnt);
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

/* Runs a DMA channel now; timings other than "immediately" are started from the events they wait for */
void rc_dma_run(int ch) {
    uint32_t base = 0xb0 + ch * 12;
    uint32_t src = io32(base), dst = io32(base + 4), cnt = io32(base + 8);
    uint32_t n = cnt & 0x1fffff, word = cnt & (1 << 26) ? 4 : 2;
    int sd = (cnt >> 23) & 3, dd = (cnt >> 21) & 3;
    uint32_t timing = (cnt >> 27) & 7;
    if (!n) n = 0x200000;
    if (timing == 7) {                                  /* geometry FIFO: hand the words to the 3D engine */
        for (uint32_t i = 0; i < n; i++, src += 4) gpu3d_write(0x400, rd32(src));
    } else {
        for (uint32_t i = 0; i < n; i++) {
            if (word == 4) wr32(dst, rd32(src)); else wr16(dst, rd16(src));
            src += sd == 0 ? word : sd == 1 ? -(int32_t)word : 0;
            dst += dd == 0 || dd == 3 ? word : dd == 1 ? -(int32_t)word : 0;
        }
    }
    set_io32(base, src);
    if (dd != 3) set_io32(base + 4, dst);
    if (!(cnt & (1u << 25)) || timing == 0) set_io32(base + 8, cnt & ~0x80000000u);
    if (cnt & (1u << 30)) rc_raise_irq(1u << (8 + ch));
}

/* Starts the channels waiting for an event: 1 VBlank, 2 HBlank, 3 start of display, 7 geometry FIFO */
void rc_dma_event(uint32_t timing) {
    for (int ch = 0; ch < 4; ch++) {
        uint32_t cnt = io32(0xb8 + ch * 12);
        if ((cnt & 0x80000000u) && ((cnt >> 27) & 7) == timing) rc_dma_run(ch);
    }
}

static void dma_written(int ch) {
    uint32_t cnt = io32(0xb8 + ch * 12);
    if (!(cnt & 0x80000000u)) return;
    uint32_t timing = (cnt >> 27) & 7;
    if (timing == 0 || timing == 7) rc_dma_run(ch);       /* the geometry FIFO is always ready for more */
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
        if (cnt & (1u << 30)) rc_raise_irq(1u << (8 + ch));
    }
}

static uint32_t io_read(uint32_t off, uint32_t v) {
    if (off >= 0x100 && off < 0x110 && !(off & 2)) v = (v & 0xffff0000) | rc_timer_read((off - 0x100) / 4);
    if (off == 0x130) {
        v = (v & 0xffff0000) | fe_keys;
        static uint64_t logged;
        static uint64_t reads;
        reads++;
        static uint64_t counted;
        static int log_keys = -1;                         /* (getenv is slow, and the keys are read often) */
        if (log_keys < 0) log_keys = getenv("B2W2_LOG_KEYS") != NULL;
        if (log_keys && rc_frames % 600 == 0 && counted != rc_frames) {
            counted = rc_frames;
            rc_logf("key reads so far %llu at frame %llu\n", (unsigned long long)reads, (unsigned long long)rc_frames);
        }
        if (log_keys && fe_keys != 0x3ff && logged != rc_frames) {
            logged = rc_frames;
            rc_logf("keys read %03x at frame %llu (lr %08x)\n", fe_keys, (unsigned long long)rc_frames, rc_cpu.r[14]);
        }
    }
    if (off == 0x180) v = (v & 0xffff0000) | arm7_ipcsync_read((uint16_t)v);
    if (off == 0x184) v = arm7_fifocnt_read();
    if (off == 0x1a4) v = card_romctrl_read();
    if (off >= 0x600 && off < 0x700) v = gpu3d_read(off);
    return v;
}

/* ------------------------------------------------------------------ debugging: watched main RAM */

/* B2W2_WATCH=lo-hi,a1-a2,b1-b2 (main RAM offsets in hex, frames in decimal) takes the pages of main RAM lo-hi off the
 * fast path, then logs every byte read between frames b1 and b2 that was never read between a1 and a2, to find what
 * one action reads that another doesn't. B2W2_DUMP_RAM=frame writes main RAM to ram_<frame>.bin. */
static unsigned long long watch_a1, watch_a2, watch_b1, watch_b2;
static uint8_t watch_seen[4 << 20];
static uint32_t watch_lr[4 << 20];

static int watched(uint32_t a) {
    if (!watch_hi || a < 0x02000000 || a >= 0x03000000) return 0;
    uint32_t m = (a - 0x02000000) & 0x3fffff;
    return m >= (watch_lo & ~0xfffu) && m < ((watch_hi + 0xfff) & ~0xfffu);
}
static uint8_t *watch_read(uint32_t a, int n) {
    uint32_t m = (a - 0x02000000) & 0x3fffff;
    /* Inside the game's event flag test (func_02019194: flag in r4, lr set by its call to the byte lookup): log each
     * flag tested in the second window once, with where it lives and whether it is set */
    static uint8_t flag_seen[0x4000];
    if (n == 1 && rc_cpu.r[14] == 0x0201919du && rc_cpu.r[4] < 0x4000 && rc_frames >= watch_a1 && rc_frames < watch_a2)
        flag_seen[rc_cpu.r[4]] |= 1;
    if (n == 1 && rc_cpu.r[14] == 0x0201919du && rc_frames >= watch_b1 && rc_frames < watch_b2) {
        uint32_t flag = rc_cpu.r[4];
        if (flag < 0x4000 && !(flag_seen[flag] & 3) && (flag_seen[flag] |= 2))
            rc_logf("flag %4u (0x%04x) at %08x bit %u = %d (frame %llu)\n", flag, flag, a, flag & 7,
                    (main_ram[m] >> (flag & 7)) & 1, (unsigned long long)rc_frames);
    }
    for (int i = 0; i < n; i++) {
        uint32_t b = m + (uint32_t)i;
        if (rc_frames >= watch_a1 && rc_frames < watch_a2) watch_seen[b] |= 1;
        if (rc_frames >= watch_b1 && rc_frames < watch_b2 && !(watch_seen[b] & 2)) { watch_seen[b] |= 2; watch_lr[b] = rc_cpu.r[14]; }
    }
    return main_ram + m;
}

/* ------------------------------------------------------------------ game tweaks */

/* Running shoes from the start: system flag 2403 (the field reads it before letting B run) is set every frame. The
 * event flags hang off the game data, whose pointer is a global: [0x02140af4] + 0x25c = event work, whose flag bits
 * start at +0x35e (func_02019234's offset). New games clear the flags; the next frame sets this one again, and it is
 * saved like any other flag. B2W2_NO_RUNNING_SHOES=1 leaves the game alone. */
#define RUNNING_SHOES_FLAG 2403
static int in_main_ram(uint32_t a) { return a >= 0x02000000 && a < 0x02400000; }
static void give_running_shoes(void) {
    static int off = -1;
    if (off < 0) off = getenv("B2W2_NO_RUNNING_SHOES") != NULL;
    if (off) return;
    uint32_t game = rd32(0x02140af4), ev = in_main_ram(game) ? rd32(game + 0x25c) : 0;
    if (!in_main_ram(ev)) return;
    uint32_t a = ev + 0x35e + RUNNING_SHOES_FLAG / 8;
    uint8_t bit = (uint8_t)(1 << (RUNNING_SHOES_FLAG & 7));
    if (!(rd8(a) & bit)) wr8(a, rd8(a) | bit);
}

/* Once a frame, after VBlank starts */
void rc_debug_frame(void) {
    give_running_shoes();
    static int init;
    static uint32_t lo, hi;
    if (!init) {
        init = 1;
        const char *w = getenv("B2W2_WATCH");
        if (!w || sscanf(w, "%x-%x,%llu-%llu,%llu-%llu", &lo, &hi, &watch_a1, &watch_a2, &watch_b1, &watch_b2) != 6) hi = 0;
    }
    if (hi && !watch_hi && rc_frames + 1 >= watch_a1) { watch_lo = lo; watch_hi = hi; remap(); }   /* slow from here */
    const char *pk = getenv("B2W2_POKE");             /* B2W2_POKE=frame:addr=byte,... (hex address and byte) */
    while (pk && *pk) {
        unsigned long long f = strtoull(pk, (char **)&pk, 10);
        unsigned addr = 0, val = 0;
        if (*pk == ':' && sscanf(pk + 1, "%x=%x", &addr, &val) == 2 && rc_frames == f) wr8(addr, (uint8_t)val);
        pk = strchr(pk, ',');
        if (pk) pk++;
    }
    const char *d = getenv("B2W2_DUMP_RAM");
    if (d && rc_frames == strtoull(d, NULL, 10)) {
        char name[64];
        sprintf(name, "ram_%llu.bin", (unsigned long long)rc_frames);
        FILE *f = fopen(name, "wb");
        if (f) { fwrite(main_ram, 1, sizeof main_ram, f); fclose(f); }
    }
    if (watch_hi && rc_frames == watch_b2) {
        int n = 0;
        for (uint32_t m = watch_lo; m < watch_hi && n < 400; m++)
            if ((watch_seen[m] & 3) == 2) {
                rc_logf("watch: %08x = %02x (lr %08x)\n", 0x02000000 + m, main_ram[m], watch_lr[m]);
                n++;
            }
        rc_logf("watch: %d addresses read only in the second window\n", n);
    }
}

uint32_t rc_io_rd32(uint32_t a) {
    uint32_t v;
    if (watched(a)) { memcpy(&v, watch_read(a & ~3u, 4), 4); return v; }
    if (a >= 0x04000000 && a < 0x04010000) v = io_read(a & 0xffff, io32(a & 0xffff));
    else if (a == 0x04100000) v = arm7_fifo_read();
    else if (a == 0x04100010) v = card_data_read();
    else if (a >= 0x08000000 && a < 0x0a000000) v = 0xffffffff;       /* empty GBA slot */
    else v = 0;
    note_io(a, 0, v);
    return v;
}
uint16_t rc_io_rd16(uint32_t a) {
    uint16_t v;
    if (watched(a)) { memcpy(&v, watch_read(a & ~1u, 2), 2); return v; }
    if (a >= 0x04000000 && a < 0x04010000) {
        uint32_t off = a & 0xffff;
        v = (uint16_t)(io_read(off & ~3u, io32(off & ~3u)) >> ((off & 2) * 8));
        if (off & 2) {
            uint32_t hi = io_read(off, io16(off));
            v = (uint16_t)hi;
        }
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
    if (watched(a)) return *watch_read(a, 1);
    return (uint8_t)(rc_io_rd16(a & ~1u) >> ((a & 1) * 8));
}

static void io_written(uint32_t off, uint32_t len) {
    for (uint32_t o = off; o < off + len; o++) {
        if (o >= 0x240 && o <= 0x249 && o != 0x247) { rc_remap_vram(); break; }
    }
    if (off < 0x2a0 && off + len > 0x280) hw_divide();
    if (off < 0x2c0 && off + len > 0x2b0) hw_sqrt();
    for (int ch = 0; ch < 4; ch++)
        if (off < 0xbc + ch * 12u && off + len > 0xba + ch * 12u) dma_written(ch);
    for (int t = 0; t < 4; t++)
        if (off < 0x104 + t * 4u && off + len > 0x100 + t * 4u) rc_timer_write(t, off);
    if ((off <= 0x208 && off + len > 0x208) || (off < 0x214 && off + len > 0x210)) rc_irq_enabled(&rc_cpu);
    if (off <= 0x180 && off + len > 0x180) arm7_ipcsync_write(io16(0x180));
    if (off <= 0x184 && off + len > 0x184) arm7_fifocnt_write(io16(0x184));
    if (off == 0x188) arm7_fifo_write(io32(0x188));
    if (off < 0x1b0 && off + len > 0x1a8) for (int i = 0; i < 8; i++) card_cmd_write(i, rc_io[0x1a8 + i]);
    if (off <= 0x1a4 && off + len > 0x1a7) card_romctrl_write(io32(0x1a4));
    if (off <= 0x1a2 && off + len > 0x1a2 && (io16(0x1a0) & 0x8000))       /* card SPI: the reply replaces the byte */
        rc_io[0x1a2] = card_spi_write(io16(0x1a0), rc_io[0x1a2]);
}

static int io_special_write(uint32_t off, uint32_t v, uint32_t len) {
    if (off >= 0x214 && off < 0x218) {                  /* IF: writing 1 acknowledges */
        uint32_t mask = len == 4 ? v : len == 2 ? v << ((off & 2) * 8) : v << ((off & 3) * 8);
        set_io32(0x214, io32(0x214) & ~mask);
        return 1;
    }
    if (off >= 0x320 && off < 0x700) {                  /* 3D engine */
        if (len == 4) gpu3d_write(off, v);
        else {
            uint32_t w = io32(off & ~3u);
            uint32_t sh = (off & 3) * 8, m = (len == 2 ? 0xffffu : 0xffu) << sh;
            w = (w & ~m) | ((v << sh) & m);
            set_io32(off & ~3u, w);
            if (off >= 0x400) gpu3d_write(off & ~3u, w);
            else gpu3d_write(off & ~3u, w);
        }
        return 1;
    }
    return 0;
}

void rc_io_wr32(uint32_t a, uint32_t v) {
    if (watched(a)) { memcpy(main_ram + ((a - 0x02000000) & 0x3ffffc), &v, 4); return; }
    note_io(a, 1, v);
    if (a < 0x04000000 || a >= 0x04010000) return;
    uint32_t off = a & 0xffff;
    if (io_special_write(off, v, 4)) return;
    set_io32(off, v);
    io_written(off, 4);
}
void rc_io_wr16(uint32_t a, uint16_t v) {
    if (watched(a)) { memcpy(main_ram + ((a - 0x02000000) & 0x3ffffe), &v, 2); return; }
    note_io(a, 1, v);
    if (a < 0x04000000 || a >= 0x04010000) return;
    uint32_t off = a & 0xffff;
    if (io_special_write(off, v, 2)) return;
    set_io16(off, v);
    io_written(off, 2);
}
void rc_io_wr8(uint32_t a, uint8_t v) {
    if (watched(a)) { main_ram[(a - 0x02000000) & 0x3fffff] = v; return; }
    note_io(a, 1, v);
    if (a < 0x04000000 || a >= 0x04010000) return;
    uint32_t off = a & 0xffff;
    if (io_special_write(off, v, 1)) return;
    rc_io[off] = v;
    io_written(off, 1);
}

/* ------------------------------------------------------------------ dispatch */

static RcFn lookup(const RcModule *m, uint32_t addr) {
    uint32_t lo = 0, hi = m->count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (m->funcs[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    return lo < m->count && m->funcs[lo].addr == addr ? m->funcs[lo].fn : NULL;
}

/* Code that is copied or loaded before it runs (code copies, overlays) applies while guest memory holds the bytes it
 * was recompiled from. Comparing the start of the module and of the function tells overlays at one address apart. */
static int module_present(const RcModule *m, uint32_t fn, int loose) {
    if (m->copy_src) {
        for (uint32_t i = 0; i < m->end - m->start; i += 4)
            if (rd32(m->start + i) != rd32(m->copy_src + i)) return 0;
        return 1;
    }
    if (m->overlay < 0) return 1;
    if (!loose)
        for (uint32_t i = 0; i < m->sig_len; i += 4)
            if (rd32(m->start + i) != m->data[i / 4]) return 0;
    if (fn + 8 <= m->end && m->data)
        for (uint32_t i = 0; i < 8; i += 4)
            if (rd32(fn + i) != m->data[(fn - m->start + i) / 4]) return 0;
    return 1;
}

static const RcModule *last_overlay;
const char *rc_last_overlay(void) { return last_overlay ? last_overlay->name : "-"; }

/* Screens the runtime draws differently, known by a function of theirs that the game calls (through a pointer) once a
 * frame while that screen is up. rc_scene_frame[]: frame + 1 of the last call, 0 = never. */
static const struct { int overlay; uint32_t addr; int scene; } scene_marks[] = {
    {162, 0x0219d3a4, SCENE_TITLE_MENU},
    {167, 0x021b3028, SCENE_BATTLE_COMMAND}, {167, 0x021b3600, SCENE_BATTLE_MOVES},
    {167, 0x021b3a24, SCENE_BATTLE_BAG}, {167, 0x021b392c, SCENE_BATTLE_PARTY},
    {167, 0x021b5aec, SCENE_BATTLE_PARTY},                          /* the party after a faint: who goes next */
    {167, 0x021b5930, SCENE_BATTLE_NEXT},                           /* before it: use the next one, or run */
    {36, 0x02197e00, SCENE_FIELD_MENU},
    {167, 0x021b13d4, SCENE_BATTLE}, {87, 0x021e9fc0, SCENE_REPORT},
};
uint64_t rc_scene_frame[SCENE_COUNT];
uint64_t rc_title_menu_frame;
void fe_report_event(uint32_t state);             /* frontend.c */

/* Calls the runtime steps into: the report's event (overlay 12), handed the address of its state in r1 */
static void call_hooks(Cpu *c, const RcModule *m, uint32_t a) {
    if (a == 0x0215bf00u && m->overlay == 12) fe_report_event(c->r[1]);
}

static void note_call(const RcModule *m, uint32_t a) {
    for (int i = 0; i < (int)(sizeof scene_marks / sizeof *scene_marks); i++)
        if (a == scene_marks[i].addr && m->overlay == scene_marks[i].overlay) {
            int sc = scene_marks[i].scene;
            static int log_scenes = -1;
            if (log_scenes < 0) log_scenes = getenv("B2W2_LOG_SCENES") != NULL;
            if (log_scenes && rc_scene_frame[sc] < rc_frames) rc_logf("scene %d frame %llu\n", sc, (unsigned long long)rc_frames);
            rc_scene_frame[sc] = rc_frames + 1;
        }
    rc_title_menu_frame = rc_scene_frame[SCENE_TITLE_MENU];
}

int rc_scene(int scene) { return rc_scene_frame[scene] && rc_frames <= rc_scene_frame[scene] + 2; }

extern volatile uint32_t rc_last_call;

/* debug: a callee must leave sp as it found it; report the ones that don't (unless a thread switch happened) */
uint32_t rc_current_thread(void);
static void checked_call(Cpu *c, RcFn fn, uint32_t a) {
    uint32_t sp0 = c->r[13], mode0 = c->ctl & 0x1f, th0 = rc_current_thread();
    fn(c);
    static int reports, check = -1;                     /* (looked up once: this is every call through a pointer) */
    if (check < 0) check = getenv("B2W2_CHECK_SP") != NULL;
    if (check && reports < 30 && mode0 == 0x1f && (c->ctl & 0x1f) == mode0 && c->r[13] != sp0 &&
        rc_current_thread() == th0) {
        reports++;
        rc_logf("sp changed by call to %08x: %08x -> %08x (lr %08x, frame %llu)\n", a, sp0, c->r[13], c->r[14],
                (unsigned long long)rc_frames);
    }
}

/* The calls through pointers seen lately into resident code and code copies, by address: the module and function
 * found for each. A resident module's is kept only where no code copy could also be (copies come first); a copy is
 * checked as every time. Overlays aren't kept: two at one address can hold the same bytes there, and only the order
 * below (the last one used first) tells them apart. */
static struct { uint32_t a; const RcModule *m; RcFn fn; } call_cache[1024];

void rc_call(Cpu *c, uint32_t addr) {
    uint32_t a = addr & ~1u;
    rc_last_call = a;
    unsigned slot = (a >> 1) & 1023;
    if (call_cache[slot].a == a) {
        const RcModule *m = call_cache[slot].m;
        if (m->overlay < 0 && !m->copy_src) { note_call(m, a); call_hooks(c, m, a); checked_call(c, call_cache[slot].fn, a); return; }
        if (module_present(m, a, 0)) {
            note_call(m, a);
            call_hooks(c, m, a);
            checked_call(c, call_cache[slot].fn, a);
            return;
        }
    }
    /* Most indirect calls into overlays hit the same one as last time */
    if (last_overlay && a >= last_overlay->start && a < last_overlay->end) {
        RcFn fn = lookup(last_overlay, a);
        if (fn && module_present(last_overlay, a, 0)) { note_call(last_overlay, a); call_hooks(c, last_overlay, a); checked_call(c, fn, a); return; }
    }
    /* Passes: code copies, resident modules, overlays matching at their start and at the function, then overlays
     * matching at the function only (DS Protect keeps the start of overlay 337 encrypted in RAM) */
    for (int pass = 0; pass < 4; pass++)
    for (int i = 0; i < rc_module_count; i++) {
        const RcModule *m = &rc_modules[i];
        int kind = m->copy_src ? 0 : m->overlay < 0 ? 1 : 2;
        if (kind != (pass == 3 ? 2 : pass) || a < m->start || a >= m->end) continue;
        RcFn fn = lookup(m, a);
        if (!fn || !module_present(m, a, pass == 3)) continue;
        if (kind == 2) last_overlay = m;
        int cache = kind != 2;
        if (kind == 1)
            for (int j = 0; j < rc_module_count; j++)
                if (rc_modules[j].copy_src && a >= rc_modules[j].start && a < rc_modules[j].end) cache = 0;
        if (cache) { call_cache[slot].a = a; call_cache[slot].m = m; call_cache[slot].fn = fn; }
        note_call(m, a);
        call_hooks(c, m, a);
        checked_call(c, fn, a);
        return;
    }
    for (int i = 0; i < rc_module_count; i++) {
        const RcModule *m = &rc_modules[i];
        if (m->overlay < 0 || a < m->start || a >= m->end || !m->data) continue;
        rc_logf("candidate %s %08x-%08x: memory %08x %08x at start, %08x %08x at target; module %08x %08x / %08x %08x\n",
                m->name, m->start, m->end, rd32(m->start), rd32(m->start + 4), rd32(a), rd32(a + 4), m->data[0],
                m->data[1], m->data[(a - m->start) / 4], m->data[(a - m->start) / 4 + 1]);
    }
    rc_fatal(c, "call to %08x: no recompiled function there (lr %08x)", addr, c->r[14]);
}

/* DS Protect's call wrappers (the recompiler starts each with this). Given a function (r0), on a genuine card one
 * checks the card and the hardware, then calls it with the next two arguments and returns what it returns. Its checks
 * can't pass recompiled (they hash and decrypt code the recompiled program doesn't run from), and when they fail it
 * ends the game, so the call is made directly. Given anything else (the wrappers that run when a check has already
 * failed), the wrapper runs as it is: 0. */
int rc_dsprot_call(Cpu *c) {
    uint32_t fn = c->r[0], a = fn & ~1u, ra = c->r[14];
    for (int i = 0; i < rc_module_count; i++) {
        const RcModule *m = &rc_modules[i];
        if (a < m->start || a >= m->end || !lookup(m, a) || !module_present(m, a, 0)) continue;
        c->r[0] = c->r[1];
        c->r[1] = c->r[2];
        rc_call(c, fn);
        c->r[14] = ra;
        c->ret_to = ra;
        return 1;
    }
    return 0;
}

void rc_bad_jump(Cpu *c, uint32_t at, uint32_t target) { rc_fatal(c, "computed jump at %08x to %08x has no case", at, target); }
void rc_unimpl(Cpu *c, uint32_t at, const char *what) { rc_fatal(c, "unimplemented instruction at %08x: %s", at, what); }
void rc_trap(Cpu *c, uint32_t at) { rc_fatal(c, "breakpoint/undefined instruction at %08x", at); }

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

void rc_set_cpsr(Cpu *c, uint32_t cpsr) {
    c->n = cpsr >> 31; c->z = (cpsr >> 30) & 1; c->c = (cpsr >> 29) & 1; c->v = (cpsr >> 28) & 1; c->q = (cpsr >> 27) & 1;
    set_ctl(c, cpsr & 0xff);
}

void rc_irq_enabled(Cpu *c);

void rc_msr(Cpu *c, int spsr, uint32_t mask, uint32_t v) {
    if (spsr) { c->spsr = (c->spsr & ~mask) | (v & mask); return; }
    uint32_t old = c->ctl;
    uint32_t cpsr = (rc_get_cpsr(c) & ~mask) | (v & mask);
    c->n = cpsr >> 31; c->z = (cpsr >> 30) & 1; c->c = (cpsr >> 29) & 1; c->v = (cpsr >> 28) & 1; c->q = (cpsr >> 27) & 1;
    if (mask & 0xff) set_ctl(c, cpsr & 0xff);
    if ((old & 0x80) && !(c->ctl & 0x80)) rc_irq_enabled(c);
}

uint32_t rc_get_user_reg(Cpu *c, int r) {
    if (mode_slot(c->ctl) == 0) return c->r[r];
    return r == 13 ? c->bank_sp[0] : c->bank_lr[0];
}
void rc_set_user_reg(Cpu *c, int r, uint32_t v) {
    if (mode_slot(c->ctl) == 0) c->r[r] = v;
    else if (r == 13) c->bank_sp[0] = v;
    else c->bank_lr[0] = v;
}

/* movs pc, lr / subs pc, lr, #4 / ldm ..pc^: the target is in r15. When it lands in another guest thread (the SDK's
 * context load), carry on in that thread's fiber. */
void rc_exception_return(Cpu *c) {
    rc_set_cpsr(c, c->spsr);
    rc_thread_check(c, c->r[15]);
}

/* ------------------------------------------------------------------ CP15 */

void rc_mcr(Cpu *c, int cp, int opc1, int crn, int crm, int opc2, uint32_t v) {
    (void)opc1;
    if (cp != 15) { rc_logf("mcr p%d ignored\n", cp); return; }
    cp15_regs[crn & 15][crm & 15][opc2 & 7] = v;
    if (crn == 1 && crm == 0) cp15_ctl = v;
    if (crn == 7 && crm == 0 && opc2 == 4) rc_halt(c, c->r[15]);      /* wait for interrupt */
    if (crn == 9 && crm == 1 && opc2 == 0) {
        rc_dtcm_base = v & 0xfffff000;
        rc_logf("cp15: DTCM at %08x (%08x)\n", rc_dtcm_base, v);
        remap();
    }
    if (crn == 9 && crm == 1 && opc2 == 1) {
        itcm_size = 512u << ((v >> 1) & 0x1f);
        rc_logf("cp15: ITCM size %08x (%08x)\n", itcm_size, v);
        remap();
    }
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

/* IntrWait as the BIOS does it: wait until the IRQ handler has set one of the wanted bits in the check flags at the
 * top of DTCM (the SDK's handlers do that), then clear them */
static void intr_wait(Cpu *c, uint32_t discard, uint32_t mask, uint32_t at) {
    uint32_t flags = rc_dtcm_base + 0x3ff8;
    if (discard) wr32(flags, rd32(flags) & ~mask);
    for (;;) {
        uint32_t f = rd32(flags);
        if (f & mask) { wr32(flags, f & ~mask); return; }
        set_io32(0x208, 1);
        rc_halt(c, at + 2);
    }
}

void rc_swi(Cpu *c, uint32_t n, uint32_t at) {
    switch (n) {
    case 0x03: rc_advance(c->r[0] * 4u / 2); break;               /* WaitByLoop */
    case 0x04: intr_wait(c, c->r[0], c->r[1], at); break;         /* IntrWait */
    case 0x05: intr_wait(c, 1, 1, at); break;                     /* VBlankIntrWait */
    case 0x06: rc_halt(c, at + 2); break;                          /* Halt */
    case 0x09: {                                                   /* Div */
        int32_t num = (int32_t)c->r[0], den = (int32_t)c->r[1];
        if (den == 0) rc_fatal(c, "SWI Div by zero at %08x", at);
        c->r[0] = (uint32_t)(num / den); c->r[1] = (uint32_t)(num % den);
        c->r[3] = (uint32_t)abs(num / den);
        break;
    }
    case 0x0b: case 0x0c: {                                        /* CpuSet, CpuFastSet */
        uint32_t src = c->r[0], dst = c->r[1], cnt = c->r[2], len = cnt & 0x1fffff;
        int fill = (cnt >> 24) & 1, word = n == 0x0c || (cnt >> 26) & 1;
        for (uint32_t i = 0; i < len; i++) {
            if (word) wr32(dst + i * 4, rd32(fill ? src : src + i * 4));
            else wr16(dst + i * 2, rd16(fill ? src : src + i * 2));
        }
        break;
    }
    case 0x0d: {                                                   /* Sqrt */
        uint32_t v = c->r[0], r = 0;
        for (uint32_t bit = 1u << 15; bit; bit >>= 1)
            if ((uint64_t)(r | bit) * (r | bit) <= v) r |= bit;
        c->r[0] = r;
        break;
    }
    case 0x0e: {                                                   /* GetCRC16: CRC-16 with reflected 0x8005 */
        uint32_t crc = c->r[0] & 0xffff;
        for (uint32_t i = 0; i < c->r[2]; i++) {
            crc ^= rd8(c->r[1] + i);
            for (int j = 0; j < 8; j++) crc = crc & 1 ? (crc >> 1) ^ 0xa001 : crc >> 1;
        }
        c->r[0] = crc;
        break;
    }
    case 0x11: lz77_uncomp(c->r[0], c->r[1], 0); break;           /* LZ77UnCompReadNormalWrite8bit */
    case 0x12: lz77_uncomp(c->r[0], c->r[1], 1); break;           /* LZ77UnCompReadNormalWrite16bit */
    case 0x14: case 0x15: rl_uncomp(c->r[0], c->r[1]); break;     /* RLUnComp */
    default:
        rc_logf("swi %02x at %08x not implemented\n", n, at);
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

    uint8_t *p = vram_mem;
    for (int b = 0; b < 9; p += bank_size[b], b++) bank_ptr[b] = p;
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
    arm7_boot();

    memset(c, 0, sizeof *c);
    c->ctl = 0xdf;                          /* system mode, IRQ and FIQ disabled */
    c->r[13] = 0x03002f7c;
    c->bank_sp[2] = 0x03003f80;             /* IRQ and SVC stacks as the firmware leaves them */
    c->bank_sp[3] = 0x03003fc0;
    c->r[15] = arm9_entry;
    c->budget = 256;
    rc_logf("boot: ARM9 %u bytes at %08x, entry %08x\n", arm9_size, arm9_ram, arm9_entry);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <rom.nds>\n", argv[0]);
        return 2;
    }
    rc_log = fopen("b2w2.log", "w");
    if (rc_log) setvbuf(rc_log, NULL, _IONBF, 0);  /* MSVC has no line buffering */
    fe_init();
    rc_sched_init();
    boot(&rc_cpu, argv[1]);
    rc_call(&rc_cpu, rc_cpu.r[15]);
    fprintf(stderr, "entry function returned (lr %08x)\n", rc_cpu.r[14]);
    if (rc_log) fclose(rc_log);
    return 0;
}

/* Debug builds (RECOMP_CHECK_SP): a direct call that returns with sp moved, in system mode, without a thread
 * switch, points at a mistranslated function */
void rc_check_sp(Cpu *c, uint32_t sp0, uint32_t target) {
    static int reports;
    if (c->r[13] == sp0 || (c->ctl & 0x1f) != 0x1f || reports >= 40) return;
    reports++;
    rc_logf("direct call to %08x moved sp %08x -> %08x (frame %llu, overlay %s)\n", target, sp0, c->r[13],
            (unsigned long long)rc_frames, rc_last_overlay());
}
