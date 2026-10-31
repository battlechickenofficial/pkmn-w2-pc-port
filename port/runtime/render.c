/* The two 2D engines, drawn a scanline at a time from the registers, VRAM, palette and OAM as they are when that
 * line is drawn, so mid-frame changes (HBlank DMA, VCount interrupts) show. Follows GBATEK "DS Video": BG modes 0-5
 * (text, affine, extended), sprites, windows, blending, master brightness, the VRAM display mode, display capture,
 * and engine A's BG0 showing the 3D engine's output. Colours are 15-bit BGR until the final conversion. */
#include "render.h"

static int engine;             /* 0 = A, 1 = B */
static uint32_t eo;            /* I/O offset of the engine's registers */
static uint32_t dispcnt;

static uint16_t pal(uint32_t off) { uint16_t v; memcpy(&v, rc_palette + ((off + (engine ? 0x400 : 0)) & 0x7ff), 2); return v; }
static uint32_t bg_vram(void) { return engine ? 0x06200000 : 0x06000000; }
static uint32_t obj_vram(void) { return engine ? 0x06600000 : 0x06400000; }
static uint16_t ext16(uint8_t *p, uint32_t i) { uint16_t v; memcpy(&v, p + i * 2, 2); return v; }

/* Affine BG reference points: latched from BGxX/BGxY at the start of a frame or when the game writes them,
 * then moved by PB/PD after each line */
static int32_t aff_x[2][2], aff_y[2][2];
static uint32_t aff_raw[2][2][2];

static void put(Line *l, int x, uint16_t c, uint8_t prio, uint8_t flags, uint8_t alpha) {
    int i = AT(x);
    l->color[i] = c & 0x7fff; l->prio[i] = prio; l->flags[i] = flags | OPAQUE; l->alpha[i] = alpha;
}

/* Widescreen margins of a tiled BG. Its map wraps at 256 or 512 pixels, so drawing it on would bring the far side of
 * the screen (half a text box) into the margin. Instead: a line that repeats (grids, checks, plain colour) goes on
 * repeating. On the backmost layer, a picture that crosses the edge is mirrored there, so shapes cut by the screen
 * edge (the title menu's Poke Ball) close up round instead of being smeared sideways. Anything else carries its edge
 * pixel outward, which leaves the margin empty when the edge is empty. */
static void extend_margins(Line *l, int mirror_left, int mirror_right) {
    int period = 0;
    for (int p = 8; p <= 128 && !period; p += 8) {
        int ok = 1;
        for (int x = 0; x + p < 256 && ok; x++) {
            int a = AT(x), b = AT(x + p);
            if (l->flags[a] != l->flags[b] ||
                ((l->flags[a] & OPAQUE) && (l->color[a] != l->color[b] || l->prio[a] != l->prio[b]))) ok = 0;
        }
        if (ok) period = p;
    }
    for (int x = X0; x < X1; x++) {
        if (x == 0) x = 256;
        int src = x;
        int edge = AT(x < 0 ? 0 : 255);
        if (period) { while (src < 0) src += period; while (src > 255) src -= period; }
        else if ((x < 0 ? mirror_left : mirror_right) && (l->flags[edge] & OPAQUE)) src = x < 0 ? -1 - x : 511 - x;
        else src = x < 0 ? 0 : 255;
        int d = AT(x), sidx = AT(src);
        l->color[d] = l->color[sidx]; l->prio[d] = l->prio[sidx]; l->flags[d] = l->flags[sidx]; l->alpha[d] = l->alpha[sidx];
    }
}

/* Whether a picture crosses a BG's left or right edge: more than two colours down that column over the last frame.
 * A plain or striped edge is background, where mirroring would only bring text near the edge out into the margin. */
static uint16_t edge_seen[2][4][2][3];
static uint8_t edge_colours[2][4][2], edge_busy[2][4][2];

static void edge_stats(const Line *l, int n, int y) {
    for (int side = 0; side < 2; side++) {
        uint8_t *k = &edge_colours[engine][n][side];
        uint16_t *seen = edge_seen[engine][n][side];
        if (y == 0) { edge_busy[engine][n][side] = *k > 2; *k = 0; }
        int i = AT(side ? 255 : 0), j;
        uint16_t c = l->flags[i] & OPAQUE ? l->color[i] : 0x8000;
        for (j = 0; j < *k && seen[j] != c; j++);
        if (j == *k && *k < 3) seen[(*k)++] = c;
    }
}

/* ------------------------------------------------------------------ backgrounds */

static uint16_t bg_color256(int n, uint16_t cnt, uint32_t idx, uint32_t palno) {
    if (dispcnt & (1u << 30)) {                       /* extended palettes: slot n, BG0/1 may use slots 2/3 */
        int slot = n;
        if (n < 2 && (cnt & 0x2000)) slot = n + 2;
        uint8_t *p = rc_bg_extpal(engine, slot);
        if (p) return ext16(p, palno * 256 + idx);
    }
    return pal(idx * 2);
}

static void bg_text(Line *l, int n, int y, int backmost) {
    uint16_t cnt = io16(eo + 8 + 2 * n);
    int hofs = io16(eo + 0x10 + 4 * n) & 0x1ff, vofs = io16(eo + 0x12 + 4 * n) & 0x1ff;
    int w = cnt & 0x4000 ? 512 : 256, h = cnt & 0x8000 ? 512 : 256;
    uint32_t chr = bg_vram() + ((cnt >> 2) & 15) * 0x4000 + (engine ? 0 : ((dispcnt >> 24) & 7) * 0x10000);
    uint32_t scr = bg_vram() + ((cnt >> 8) & 31) * 0x800 + (engine ? 0 : ((dispcnt >> 27) & 7) * 0x10000);
    int ty = (y + vofs) & (h - 1);
    for (int x = 0; x < 256; x++) {
        int tx = (x + hofs) & (w - 1);
        uint32_t block = (tx >> 8) + (ty >> 8) * (w >> 8);
        uint16_t e = rd16(scr + block * 0x800 + ((ty & 255) >> 3) * 64 + ((tx & 255) >> 3) * 2);
        int px = tx & 7, py = ty & 7;
        if (e & 0x400) px = 7 - px;
        if (e & 0x800) py = 7 - py;
        uint32_t tile = e & 0x3ff;
        if (cnt & 0x80) {
            uint8_t idx = rd8(chr + tile * 64 + py * 8 + px);
            if (idx) put(l, x, bg_color256(n, cnt, idx, e >> 12), cnt & 3, 0, 31);
        } else {
            uint8_t b = rd8(chr + tile * 32 + py * 4 + px / 2);
            uint8_t idx = px & 1 ? b >> 4 : b & 15;
            if (idx) put(l, x, pal(((e >> 12) * 16 + idx) * 2), cnt & 3, 0, 31);
        }
    }
    edge_stats(l, n, y);
    extend_margins(l, backmost && edge_busy[engine][n][0], backmost && edge_busy[engine][n][1]);
}

/* Affine and extended BGs: walk the texture with the rotation/scaling parameters */
static void bg_affine(Line *l, int n, int y, int extended) {
    uint16_t cnt = io16(eo + 8 + 2 * n);
    uint32_t ar = eo + (n == 2 ? 0x20 : 0x30);
    int32_t pa = (int16_t)io16(ar), pb = (int16_t)io16(ar + 2), pc = (int16_t)io16(ar + 4), pd = (int16_t)io16(ar + 6);
    /* The internal reference point, which the hardware advances by PB/PD every line (render_engine_line) */
    int32_t x0 = aff_x[engine][n - 2], y0 = aff_y[engine][n - 2];
    (void)pb; (void)pd; (void)y;
    int wrap = cnt & 0x2000, size = cnt >> 14;
    uint32_t chr = bg_vram() + ((cnt >> 2) & 15) * 0x4000 + (engine ? 0 : ((dispcnt >> 24) & 7) * 0x10000);
    uint32_t scr = bg_vram() + ((cnt >> 8) & 31) * 0x800 + (engine ? 0 : ((dispcnt >> 27) & 7) * 0x10000);
    int kind = 0, w = 128 << size, h = 128 << size;          /* 0 8-bit map, 1 16-bit map, 2 256-colour, 3 direct */
    if (extended) {
        if (!(cnt & 0x80)) kind = 1;
        else {
            kind = cnt & 4 ? 3 : 2;
            static const int bw[4] = {128, 256, 512, 512}, bh[4] = {128, 256, 256, 512};
            w = bw[size]; h = bh[size];
            scr = bg_vram() + ((cnt >> 8) & 31) * 0x4000;
        }
    }
    for (int x = X0; x < X1; x++) {                          /* the transform carries on into the margins */
        int tx = (x0 + pa * x) >> 8, ty = (y0 + pc * x) >> 8;
        if (wrap) { tx &= w - 1; ty &= h - 1; }
        else if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
        switch (kind) {
        case 0: {
            uint8_t tile = rd8(scr + (ty >> 3) * (w >> 3) + (tx >> 3));
            uint8_t idx = rd8(chr + tile * 64 + (ty & 7) * 8 + (tx & 7));
            if (idx) put(l, x, pal(idx * 2), cnt & 3, 0, 31);
            break;
        }
        case 1: {
            uint16_t e = rd16(scr + ((ty >> 3) * (w >> 3) + (tx >> 3)) * 2);
            int px = tx & 7, py = ty & 7;
            if (e & 0x400) px = 7 - px;
            if (e & 0x800) py = 7 - py;
            uint8_t idx = rd8(chr + (e & 0x3ff) * 64 + py * 8 + px);
            if (idx) put(l, x, bg_color256(n, cnt, idx, e >> 12), cnt & 3, 0, 31);
            break;
        }
        case 2: {
            uint8_t idx = rd8(scr + ty * w + tx);
            if (idx) put(l, x, pal(idx * 2), cnt & 3, 0, 31);
            break;
        }
        case 3: {
            uint16_t c = rd16(scr + (ty * w + tx) * 2);
            if (c & 0x8000) put(l, x, c, cnt & 3, 0, 31);
            break;
        }
        }
    }
}

static void bg_3d(Line *l, int y) {
    const uint32_t *fb3 = gpu3d_framebuffer();
    int hofs = (int)(io16(eo + 0x10) << 23) >> 23;
    uint8_t prio = io16(eo + 8) & 3;
    for (int x = X0; x < X1; x++) {
        int sx = x + hofs;
        if (sx < -GPU3D_OFS || sx >= 256 + GPU3D_OFS) continue;
        uint32_t p = fb3[y * GPU3D_W + sx + GPU3D_OFS];
        uint8_t a = (p >> 16) & 31;
        if (a) put(l, x, (uint16_t)p, prio, IS3D, a);
    }
}

/* ------------------------------------------------------------------ sprites */

static const uint8_t obj_w[4][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}, {0, 0, 0, 0}};
static const uint8_t obj_h[4][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}, {0, 0, 0, 0}};

static void objects(Line *l, int y) {
    const uint8_t *oam = rc_oam + (engine ? 0x400 : 0);
    for (int i = 0; i < 128; i++) {
        uint16_t a0, a1, a2;
        memcpy(&a0, oam + i * 8, 2); memcpy(&a1, oam + i * 8 + 2, 2); memcpy(&a2, oam + i * 8 + 4, 2);
        int affine = a0 & 0x100;
        if (!affine && (a0 & 0x200)) continue;
        int shape = a0 >> 14, size = a1 >> 14, mode = (a0 >> 10) & 3;
        int w = obj_w[shape][size], h = obj_h[shape][size];
        if (!w) continue;
        int bw = affine && (a0 & 0x200) ? w * 2 : w, bh = affine && (a0 & 0x200) ? h * 2 : h;
        int oy = a0 & 0xff, ox = a1 & 0x1ff;
        if (oy + bh > 256) oy -= 256;
        if (ox >= 256) ox -= 512;
        int line = y - oy;
        if (line < 0 || line >= bh) continue;
        int32_t pa = 256, pb = 0, pc = 0, pd = 256;
        if (affine) {
            int g = (a1 >> 9) & 31;
            int16_t v;
            memcpy(&v, oam + g * 32 + 6, 2); pa = v;
            memcpy(&v, oam + g * 32 + 14, 2); pb = v;
            memcpy(&v, oam + g * 32 + 22, 2); pc = v;
            memcpy(&v, oam + g * 32 + 30, 2); pd = v;
        }
        uint8_t prio = (a2 >> 10) & 3;
        uint32_t tile = a2 & 0x3ff;
        int c256 = a0 & 0x2000;
        for (int px = 0; px < bw; px++) {
            int sx = ox + px;
            if (sx < 0 || sx >= 256) continue;
            int tx, ty;
            if (affine) {
                int dx = px - bw / 2, dy = line - bh / 2;
                tx = ((pa * dx + pb * dy) >> 8) + w / 2;
                ty = ((pc * dx + pd * dy) >> 8) + h / 2;
                if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
            } else {
                tx = a1 & 0x1000 ? w - 1 - px : px;
                ty = a1 & 0x2000 ? h - 1 - line : line;
            }
            uint16_t color;
            uint8_t alpha = 31;
            if (mode == 3) {                                   /* bitmap sprite */
                uint32_t addr;
                if (dispcnt & 0x40) {
                    addr = (tile << (7 + ((dispcnt >> 22) & 1))) + (ty * w + tx) * 2;
                } else {
                    uint32_t mask = dispcnt & 0x20 ? 0x1f : 0x0f;
                    addr = (tile & mask) * 0x10 + (tile & ~mask) * 0x80 + ty * (dispcnt & 0x20 ? 512 : 256) + tx * 2;
                }
                color = rd16(obj_vram() + addr);
                if (!(color & 0x8000)) continue;
                alpha = (uint8_t)(((a2 >> 12) * 2) + 1);
            } else {
                uint32_t off;
                int shift = dispcnt & 0x10 ? 5 + ((dispcnt >> 20) & 3) : 5;
                if (c256) {
                    off = dispcnt & 0x10 ? (tile << shift) + ((ty >> 3) * (w >> 3) + (tx >> 3)) * 64
                                         : (tile & ~1u) * 32 + (ty >> 3) * 1024 + (tx >> 3) * 64;
                    uint8_t idx = rd8(obj_vram() + off + (ty & 7) * 8 + (tx & 7));
                    if (!idx) continue;
                    uint8_t *ep = dispcnt & 0x80000000u ? rc_obj_extpal(engine) : NULL;
                    color = ep ? ext16(ep, (a2 >> 12) * 256 + idx) : pal(0x200 + idx * 2);
                } else {
                    off = dispcnt & 0x10 ? (tile << shift) + ((ty >> 3) * (w >> 3) + (tx >> 3)) * 32
                                         : tile * 32 + (ty >> 3) * 1024 + (tx >> 3) * 32;
                    uint8_t b = rd8(obj_vram() + off + (ty & 7) * 4 + (tx & 7) / 2);
                    uint8_t idx = tx & 1 ? b >> 4 : b & 15;
                    if (!idx) continue;
                    color = pal(0x200 + ((a2 >> 12) * 16 + idx) * 2);
                }
            }
            /* Sprites stay within the DS's 256 pixels: games park unused ones just off screen */
            if (mode == 2) { l->flags[AT(sx)] |= OBJWIN; continue; }
            if ((l->flags[AT(sx)] & OPAQUE) && l->prio[AT(sx)] <= prio) continue;
            uint8_t keep = l->flags[AT(sx)] & OBJWIN;
            put(l, sx, color, prio, (mode == 1 || mode == 3 ? SEMI : 0) | keep, alpha);
        }
    }
}

/* ------------------------------------------------------------------ compositing */

static uint16_t blend(uint16_t a, uint16_t b, int ea, int eb) {
    int r = ((a & 31) * ea + (b & 31) * eb) >> 4, g = (((a >> 5) & 31) * ea + ((b >> 5) & 31) * eb) >> 4;
    int bl = (((a >> 10) & 31) * ea + ((b >> 10) & 31) * eb) >> 4;
    if (r > 31) r = 31;
    if (g > 31) g = 31;
    if (bl > 31) bl = 31;
    return (uint16_t)(r | g << 5 | bl << 10);
}

static uint16_t brighten(uint16_t c, int ey, int up) {
    int ch[3] = {c & 31, (c >> 5) & 31, (c >> 10) & 31};
    for (int i = 0; i < 3; i++) ch[i] = up ? ch[i] + ((31 - ch[i]) * ey >> 4) : ch[i] - (ch[i] * ey >> 4);
    return (uint16_t)(ch[0] | ch[1] << 5 | ch[2] << 10);
}

static int in_window(int n, int x, int y) {
    uint16_t h = io16(eo + 0x40 + 2 * n), v = io16(eo + 0x44 + 2 * n);
    int x1 = h >> 8, x2 = h & 0xff, y1 = v >> 8, y2 = v & 0xff;
    int inx = x1 <= x2 ? x >= x1 && x < x2 : x >= x1 || x < x2;
    int iny = y1 <= y2 ? y >= y1 && y < y2 : y >= y1 || y < y2;
    return inx && iny;
}

static uint32_t to_argb(uint16_t c) {
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return 0xff000000u | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
}

/* Display capture (DISPCAPCNT, 0x04000064): source A is engine A's picture or the 3D layer, source B a VRAM bank;
 * one of them or a blend of the two goes to a VRAM bank. Games use it for motion blur (the title screen) and
 * screen transitions. Runs once per enable, then clears bit 31 like the hardware. */
static void capture(const uint32_t *graphics) {
    uint32_t cnt = io32(0x64);
    if (!(cnt & 0x80000000u)) return;
    static const int cw[4] = {128, 256, 256, 256}, ch[4] = {128, 64, 128, 192};
    int size = (cnt >> 20) & 3, w = cw[size], h = ch[size];
    uint8_t *dst = rc_vram_bank((cnt >> 16) & 3), *srcb = rc_vram_bank((dispcnt >> 18) & 3);
    uint32_t woff = ((cnt >> 18) & 3) * 0x8000, roff = ((cnt >> 26) & 3) * 0x8000;
    int src = (cnt >> 29) & 3, eva = cnt & 31, evb = (cnt >> 8) & 31;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    const uint32_t *fb3 = gpu3d_framebuffer();
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int i = y * 256 + x;
            uint16_t a;
            int aa;
            uint32_t p3 = fb3[y * GPU3D_W + x + GPU3D_OFS];
            if (cnt & (1u << 24)) { a = (uint16_t)(p3 & 0x7fff); aa = ((p3 >> 16) & 31) != 0; }
            else { a = (uint16_t)graphics[i]; aa = 1; }
            uint16_t b = 0;
            int ab = 0;
            if (!(cnt & (1u << 25)) && srcb) { b = ext16(srcb, ((roff + i * 2) & 0x1ffff) / 2); ab = b >> 15; }
            uint16_t c;
            int alpha;
            if (src == 0) { c = a; alpha = aa; }
            else if (src == 1) { c = b; alpha = ab; }
            else {
                int ea = aa ? eva : 0, eb = ab ? evb : 0;
                c = blend(a, b, ea, eb);
                alpha = (aa && eva) || (ab && evb);
            }
            if (dst) {
                uint16_t v = (uint16_t)((c & 0x7fff) | (alpha ? 0x8000 : 0));
                memcpy(dst + ((woff + i * 2) & 0x1ffff), &v, 2);
            }
        }
    set_io32(0x64, cnt & ~0x80000000u);
}

static uint32_t pre[2][256 * 192];       /* engine output before display mode and master brightness */
static uint32_t fin[2][GPU3D_W * 192];   /* what the screens show, with the widescreen margins */

static void render_engine_line(int e, int y) {
    engine = e;
    eo = e ? 0x1000 : 0;
    dispcnt = io32(eo);
    /* Affine reference points: reload at the frame start or after a write, otherwise step by PB/PD */
    for (int k = 0; k < 2; k++) {
        uint32_t ar = eo + (k == 0 ? 0x20 : 0x30);
        uint32_t rx = io32(ar + 8), ry = io32(ar + 12);
        if (y == 0 || rx != aff_raw[e][k][0]) aff_x[e][k] = (int32_t)(rx << 4) >> 4;
        if (y == 0 || ry != aff_raw[e][k][1]) aff_y[e][k] = (int32_t)(ry << 4) >> 4;
        aff_raw[e][k][0] = rx; aff_raw[e][k][1] = ry;
    }
    uint32_t *out = pre[e] + y * 256, *row = fin[e] + y * GPU3D_W, *shown = row + GPU3D_OFS;
    int display = (dispcnt >> 16) & 3;
    if (!(io16(0x304) & (e ? 0x0200 : 0x0002)) || display == 0 || (dispcnt & 0x80)) {
        for (int x = 0; x < GPU3D_W; x++) row[x] = 0xff000000;
        for (int x = 0; x < 256; x++) { out[x] = 0x7fff; shown[x] = 0xffffffff; }
    } else {
        int mode = dispcnt & 7;
        uint16_t bldcnt = io16(eo + 0x50), bldalpha = io16(eo + 0x52);
        int bmode = (bldcnt >> 6) & 3, eva = bldalpha & 31, evb = (bldalpha >> 8) & 31, evy = io16(eo + 0x54) & 31;
        if (eva > 16) eva = 16;
        if (evb > 16) evb = 16;
        if (evy > 16) evy = 16;
        int windows = (dispcnt >> 13) & 7;
        uint16_t winin = io16(eo + 0x48), winout = io16(eo + 0x4a);
        uint16_t backdrop = pal(0);
        static Line bg[4], obj;

        int back = -1;                                       /* the backmost 2D BG: highest priority value, then number */
        for (int n = 0; n < 4; n++)
            if ((dispcnt & (0x100u << n)) && !(n == 0 && !e && (dispcnt & 8)) &&
                (back < 0 || (io16(eo + 8 + 2 * n) & 3) >= (io16(eo + 8 + 2 * back) & 3))) back = n;
        for (int n = 0; n < 4; n++) {
            memset(&bg[n].flags, 0, sizeof bg[n].flags);
            if (!(dispcnt & (0x100u << n))) continue;
            if (n == 0 && !e && (dispcnt & 8)) { bg_3d(&bg[0], y); continue; }
            int type = 0;                                    /* 0 text, 1 affine, 2 extended */
            if (n == 2) type = mode == 2 || mode == 4 ? 1 : mode == 5 ? 2 : 0;
            if (n == 3) type = mode == 1 || mode == 2 ? 1 : mode >= 3 && mode <= 5 ? 2 : 0;
            if (type == 0) bg_text(&bg[n], n, y, n == back);
            else bg_affine(&bg[n], n, y, type == 2);
        }
        memset(&obj.flags, 0, sizeof obj.flags);
        if (dispcnt & 0x1000) objects(&obj, y);
        if (!e) title_menu_line(y, bg, &obj);                /* the title menu's extra button (titlemenu.c) */

        static uint16_t comp[GPU3D_W];
        for (int sx = X0; sx < X1; sx++) {
            int x = AT(sx), wx = sx < 0 ? 0 : sx > 255 ? 255 : sx;     /* margins take the windows of the edge */
            uint8_t mask = 0x3f;
            if (windows) {
                if ((windows & 1) && in_window(0, wx, y)) mask = winin & 0x3f;
                else if ((windows & 2) && in_window(1, wx, y)) mask = (winin >> 8) & 0x3f;
                else if ((windows & 4) && (obj.flags[AT(wx)] & OBJWIN)) mask = (winout >> 8) & 0x3f;
                else mask = winout & 0x3f;
            }
            /* The two frontmost layers: lower priority value first, sprites above BGs of the same priority */
            int top = 5, second = 5;
            int top_key = 1000, second_key = 1000;
            for (int n = 0; n < 5; n++) {
                Line *l = n < 4 ? &bg[n] : &obj;
                if (!(l->flags[x] & OPAQUE) || !(mask & (1 << n))) continue;
                int key = l->prio[x] * 8 + (n == 4 ? 0 : 1 + n);
                if (key < top_key) { second = top; second_key = top_key; top = n; top_key = key; }
                else if (key < second_key) { second = n; second_key = key; }
            }
            uint16_t c1 = top < 4 ? bg[top].color[x] : top == 4 ? obj.color[x] : backdrop;
            uint16_t c2 = second < 4 ? bg[second].color[x] : second == 4 ? obj.color[x] : backdrop;
            uint8_t f1 = top < 4 ? bg[top].flags[x] : top == 4 ? obj.flags[x] : 0;
            uint8_t a1 = top < 4 ? bg[top].alpha[x] : top == 4 ? obj.alpha[x] : 31;
            int first_ok = (bldcnt >> top) & 1, second_ok = (bldcnt >> (8 + second)) & 1;
            uint16_t c = c1;
            if (mask & 0x20) {
                if (f1 & IS3D) {
                    /* The 3D layer blends by its own pixel alpha, never by EVA/EVB (an opaque 3D pixel stays) */
                    if (second_ok && (bmode == 1 || a1 < 31)) c = blend(c1, c2, (a1 + 1) / 2, 16 - (a1 + 1) / 2);
                    else if (bmode == 2 && first_ok) c = brighten(c1, evy, 1);
                    else if (bmode == 3 && first_ok) c = brighten(c1, evy, 0);
                }
                else if ((f1 & SEMI) && second_ok) c = blend(c1, c2, eva, evb);
                else if (bmode == 1 && first_ok && second_ok) c = blend(c1, c2, eva, evb);
                else if (bmode == 2 && first_ok) c = brighten(c1, evy, 1);
                else if (bmode == 3 && first_ok) c = brighten(c1, evy, 0);
            }
            comp[x] = c;
            if (sx >= 0 && sx < 256) out[sx] = c;
        }

        /* What reaches the screen: the engine's picture, an LCDC bank (VRAM display) or nothing (main memory
         * display), then master brightness */
        uint8_t *bank = display == 2 && !e ? rc_vram_lcdc(0x06800000 + ((dispcnt >> 18) & 3) * 0x20000) : NULL;
        uint16_t mb = io16(eo + 0x6c);
        int mmode = mb >> 14, mf = mb & 31;
        if (mf > 16) mf = 16;
        for (int sx = X0; sx < X1; sx++) {
            uint16_t c = comp[AT(sx)];
            if (display == 2 && !e) c = bank && sx >= 0 && sx < 256 ? ext16(bank, (uint32_t)(y * 256 + sx)) : 0;
            else if (display == 3) c = 0;
            if (mmode == 1) c = brighten(c, mf, 1);
            else if (mmode == 2) c = brighten(c, mf, 0);
            row[AT(sx)] = to_argb(c);
        }
    }
    for (int k = 0; k < 2; k++) {
        uint32_t ar = eo + (k == 0 ? 0x20 : 0x30);
        aff_x[e][k] += (int16_t)io16(ar + 2);
        aff_y[e][k] += (int16_t)io16(ar + 6);
    }
}

/* Called from the video timing for each visible line, after the HBlank DMA of the line before */
void render_line(int y) {
    render_engine_line(0, y);
    render_engine_line(1, y);
}

/* At VBlank: capture this frame of engine A, and hand over both pictures */
void render_frame(uint32_t *a, uint32_t *b) {
    engine = 0;
    eo = 0;
    dispcnt = io32(0);
    capture(pre[0]);
    memcpy(a, fin[0], sizeof fin[0]);
    memcpy(b, fin[1], sizeof fin[1]);
}
