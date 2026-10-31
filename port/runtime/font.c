/* The game's own font, for what the runtime draws on the game's screens (the title menu's extra button, the
 * save-file screens). Read from the ROM when first needed: file a/0/2/3 is a NARC of fonts, and its first is the
 * 12 x 15 one the menus use. It is an NFTR whose glyph cells start with their own metrics (left, width, advance: 3
 * bytes), then the 2-bit pixels: 1 is the letter, 2 its shadow. */
#include "font.h"
#include <stdlib.h>
#include <string.h>

int card_rom_read(uint64_t addr, void *dst, uint32_t len);

static int state;                       /* 0 not tried, 1 loaded, -1 unavailable */
static uint8_t *nftr;
static uint32_t nftr_len, cglp, cell, cw, ch;
static uint16_t *glyph_of;               /* code point -> glyph + 1, 0 = none */

static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* NitroFS: the file id of a path, walking the name table */
static int file_id(const uint8_t *fnt, uint32_t fnt_len, const char *path) {
    int dir = 0;
    for (;;) {
        const char *slash = strchr(path, '/');
        size_t n = slash ? (size_t)(slash - path) : strlen(path);
        if ((uint32_t)dir * 8 + 8 > fnt_len) return -1;
        uint32_t o = u32(fnt + dir * 8);
        int id = u16(fnt + dir * 8 + 4), next = -1;
        while (o < fnt_len && fnt[o]) {
            int len = fnt[o] & 0x7f, is_dir = fnt[o] & 0x80;
            if (o + 1 + len + (is_dir ? 2 : 0) > fnt_len) return -1;
            int match = (size_t)len == n && !memcmp(fnt + o + 1, path, n);
            if (is_dir) {
                if (match && slash) { next = u16(fnt + o + 1 + len) & 0xfff; break; }
                o += 3 + len;
            } else {
                if (match && !slash) return id;
                id++;
                o += 1 + len;
            }
        }
        if (next < 0) return -1;
        dir = next;
        path = slash + 1;
    }
}

static int load(void) {
    uint8_t hdr[0x50];
    if (!card_rom_read(0, hdr, sizeof hdr)) return 0;
    uint32_t fnt_off = u32(hdr + 0x40), fnt_len = u32(hdr + 0x44), fat_off = u32(hdr + 0x48);
    uint8_t *fnt = malloc(fnt_len);
    int id = fnt && card_rom_read(fnt_off, fnt, fnt_len) ? file_id(fnt, fnt_len, "a/0/2/3") : -1;
    free(fnt);
    uint8_t fat[8];
    if (id < 0 || !card_rom_read(fat_off + (uint32_t)id * 8, fat, 8)) return 0;
    uint32_t start = u32(fat), len = u32(fat + 4) - start;
    uint8_t *narc = malloc(len);
    if (!narc || !card_rom_read(start, narc, len) || memcmp(narc, "NARC", 4)) { free(narc); return 0; }
    /* NARC: BTAF (file table), BTNF (names), GMIF (data); the first file */
    uint32_t btaf = u16(narc + 12), btnf = btaf + u32(narc + btaf + 4), gmif = btnf + u32(narc + btnf + 4);
    uint32_t a = u32(narc + btaf + 12), b = u32(narc + btaf + 16);
    if (gmif + 8 + b > len) { free(narc); return 0; }
    nftr_len = b - a;
    nftr = malloc(nftr_len);
    memcpy(nftr, narc + gmif + 8 + a, nftr_len);
    free(narc);
    /* FINF: pointers (to section + 8) to CGLP, CWDH, CMAP */
    uint32_t finf = u16(nftr + 12);
    cglp = u32(nftr + finf + 0x10) - 8;
    uint32_t cmap = u32(nftr + finf + 0x18) - 8;
    cw = nftr[cglp + 8]; ch = nftr[cglp + 9]; cell = u16(nftr + cglp + 10);
    glyph_of = calloc(0x10000, 2);
    /* CMAP blocks: first, last, type (0 run, 1 table, 2 pairs), next */
    while (cmap && cmap + 20 <= nftr_len) {
        const uint8_t *m = nftr + cmap;
        uint32_t first = u16(m + 8), last = u16(m + 10), type = u16(m + 12), next = u32(m + 16);
        const uint8_t *d = m + 20;
        if (type == 0)
            for (uint32_t c = first; c <= last; c++) glyph_of[c] = (uint16_t)(u16(d) + c - first + 1);
        else if (type == 1)
            for (uint32_t c = first; c <= last; c++) { uint16_t g = u16(d + 2 * (c - first)); if (g != 0xffff) glyph_of[c] = g + 1; }
        else
            for (uint32_t k = 0, n = u16(d); k < n; k++) glyph_of[u16(d + 2 + 4 * k)] = u16(d + 4 + 4 * k) + 1;
        cmap = next && next - 8 + 20 <= nftr_len ? next - 8 : 0;
    }
    return 1;
}

int font_ready(void) {
    if (!state) state = load() ? 1 : -1;
    return state > 0;
}

/* The glyph cell of a character, or NULL */
static const uint8_t *glyph(wchar_t c) {
    if (!font_ready() || !glyph_of[(uint16_t)c]) return NULL;
    uint32_t o = cglp + 16 + (uint32_t)(glyph_of[(uint16_t)c] - 1) * cell;
    return o + cell <= nftr_len ? nftr + o : NULL;
}

int font_text(const wchar_t *s, int x, int y, FontPut put, void *ctx) {
    for (; *s; s++) {
        const uint8_t *g = glyph(*s);
        if (!g) { x += 6; continue; }
        const uint8_t *px = g + 3;
        for (uint32_t gy = 0; gy < ch; gy++)
            for (uint32_t gx = 0; gx < cw; gx++) {
                uint32_t bit = (gy * cw + gx) * 2;
                int v = (px[bit / 8] >> (6 - bit % 8)) & 3;
                if (v) put(ctx, x + g[0] + (int)gx, y + (int)gy, v);
            }
        x += g[2];
    }
    return x;
}

int font_width(const wchar_t *s) {
    int w = 0;
    for (; *s; s++) { const uint8_t *g = glyph(*s); w += g ? g[2] : 6; }
    return w;
}
