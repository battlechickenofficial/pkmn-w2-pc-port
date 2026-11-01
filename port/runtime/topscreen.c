/* The top screen only (layout 3): what the game shows on its touch screen, brought onto the top screen. The renderer
 * keeps the touch screen's layers apart (render_layer_b); per screen, known by the game function running
 * (rc_scene), they are placed over the top screen without their background:
 *   field menu, battle bag, battle party: the whole of it, centred
 *   battle: the view further back and raised (the 3D picture scaled, the HP boxes moved up with it)
 *   battle commands: the top screen's text box is left out, and the buttons go where it was, in a row: FIGHT,
 *     POKEMON, BAG, RUN, all four in one shape after the game's BAG button (a tab with the Japanese word, a black bar
 *     with the English one between chevrons, the tab's mirror below), in each button's colours as the game draws them
 *     that frame and with its own words; the one the game's cursor is on grows, pushing the others aside, with the
 *     cursor around it; Left and Right move along the row
 *   battle moves: the same place, two rows of two slim plates, each with the move's type, name and PP
 *   party balls: under each side's HP box, all at the small size
 * The touch screen's status bar (clock, wireless, battery) is never brought over. */
#include "render.h"

enum { L_BG0 = 1, L_BG1 = 2, L_BG2 = 4, L_BG3 = 8, L_OBJ = 16 };
enum { K_A = 1, K_B = 2, K_RIGHT = 0x10, K_LEFT = 0x20, K_UP = 0x40, K_DOWN = 0x80 };

#define W GPU3D_W
#define OFS GPU3D_OFS

static int active;

static uint32_t *layer(int n) { return render_layer_b[n]; }

/* ------------------------------------------------------------------ drawing on the top screen */

static uint32_t *top;
static uint32_t fade = 255;             /* the touch screen's fade (its master brightness going down), as alpha */

/* What the runtime draws itself, faded with the touch screen */
static uint32_t own(uint32_t rgb) { return fade >= 255 ? rgb : rgb | (fade ? fade : 1) << 24; }

static uint32_t *canvas;                /* while set, put() draws here instead (canvas_w x canvas_h, from 0, 0) */
static int canvas_w, canvas_h;

/* A pixel: opaque, or blended by its alpha (a fading touch screen fades out over the top screen) */
static void put(int x, int y, uint32_t c) {
    if (canvas) {
        if (x >= 0 && x < canvas_w && y >= 0 && y < canvas_h) canvas[y * canvas_w + x] = c ? c : 0xff000000u;
        return;
    }
    if (x < -OFS || x >= 256 + OFS || y < 0 || y >= 192) return;
    uint32_t *d = &top[y * W + x + OFS], a = c >> 24;
    if (a == 0 || a == 255) { *d = c & 0xffffff; return; }
    uint32_t r = ((c >> 16 & 255) * a + (*d >> 16 & 255) * (255 - a)) / 255;
    uint32_t g = ((c >> 8 & 255) * a + (*d >> 8 & 255) * (255 - a)) / 255;
    uint32_t b = ((c & 255) * a + (*d & 255) * (255 - a)) / 255;
    *d = r << 16 | g << 8 | b;
}

/* Copy what the given layers drew in a rectangle of the touch screen to (dx, dy) on the top screen, front layer first
 * (sprites, then BGs by priority) */
static void copy_layers(int mask, int sx, int sy, int w, int h, int dx, int dy) {
    int order[5], n = 0;
    order[n++] = 4;
    for (int p = 0; p < 4; p++)
        for (int b = 0; b < 4; b++)
            if ((io16(0x1008 + 2 * b) & 3) == p) order[n++] = b;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int ux = sx + x, uy = sy + y;
            if (ux < 0 || ux >= 256 || uy < 0 || uy >= 192) continue;
            for (int i = 0; i < 5; i++) {
                if (!(mask >> order[i] & 1)) continue;
                uint32_t c = layer(order[i])[uy * 256 + ux];
                if (c) { put(dx + x, dy + y, c); break; }
            }
        }
}

/* The touch screen as composed from the layers in render_ui_mask, rows y0 to y1, at the same place on the top screen */
static void copy_composed(int y0, int y1) {
    for (int y = y0; y < y1; y++)
        for (int x = 0; x < 256; x++) {
            uint32_t c = render_ui_b[y * 256 + x];
            if (c) put(x, y, c);
        }
}

/* Whether BG3 has the screen's own background yet (the field menu's grid, the bag's and the party's Poke Ball): the
 * game's function for a screen starts while the touch screen still shows the one before */
static int background_up(void) {
    for (int y = 40; y < 160; y += 20)
        for (int x = 8; x < 256; x += 40)
            if (layer(3)[y * 256 + x]) return 1;
    return 0;
}

/* ------------------------------------------------------------------ battle commands */

enum { B_BAG, B_FIGHT, B_POKEMON, B_RUN };

/* Each button on the touch screen: the game's BAG, POKEMON and RUN buttons (BG0), and FIGHT's "たたかう" and banner
 * (BG1, on the hexagon). On the top screen they all become the same button, BW x BH, in a row where the text box was. */
static const struct { int sx, sy, w, h; } from[4] = {{0, 146, 78, 44}, {84, 77, 88, 28}, {178, 146, 78, 44}, {97, 156, 62, 36}};
#define BW 78
#define BH 41
#define GAP 3
#define GROWN 1.15f                     /* the selected button's size */
static const int slot[4] = {2, 0, 1, 3};                 /* left to right: FIGHT, POKEMON, BAG, RUN */
#define ROW_Y 145

static int row_at = -1;                 /* where on the row the cursor is, or is going (top_screen_keys) */
static int game_cursor = -1;            /* the command button the game's cursor is on, -1 when it isn't shown */
static int cursor_min_w[8];             /* the cursor's narrowest seen around each place: the brackets breathe from there */
static uint32_t corner[16][16];         /* the game's top-left bracket, copied off its sprite */
static int have_corner;

static int cursor_seen, cursor_x0, cursor_y0, cursor_x1, cursor_y1;   /* this frame's, from cursor_box */

static int is_pink(uint32_t c) { return (c & 0xffffff) == 0xef7394; }   /* (239, 115, 148): only the cursor has it */

/* In one of the cursor's four bracket sprites (16 x 16 at the corners of its box) */
static int in_cursor(int x, int y) {
    if (!cursor_seen) return 0;
    int l = x >= cursor_x0 - 2 && x < cursor_x0 + 14, r = x > cursor_x1 - 14 && x <= cursor_x1 + 2;
    int t = y >= cursor_y0 - 1 && y < cursor_y0 + 15, b = y > cursor_y1 - 15 && y <= cursor_y1 + 1;
    return (l || r) && (t || b);
}
static int is_cursor(uint32_t c) {
    c &= 0xffffff;
    return c == 0xef7394 || c == 0xf7f7f7 || c == 0xc61029 || c == 0x636363;
}

/* The game's cursor: four bracket sprites around a button, each with a pink line inside its white edge. Its box,
 * or 0 when it isn't shown; keeps a copy of its top-left bracket. */
static int cursor_box(int *bx0, int *by0, int *bx1, int *by1) {
    const uint32_t *o = layer(4);
    int x0 = 256, y0 = 192, x1 = -1, y1 = -1;
    for (int y = 0; y < 192; y++)
        for (int x = 0; x < 256; x++)
            if (is_pink(o[y * 256 + x])) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
    cursor_seen = x1 >= 0;
    if (x1 < 0) return 0;
    cursor_x0 = x0; cursor_y0 = y0; cursor_x1 = x1; cursor_y1 = y1;
    /* the top-left bracket: its pink starts 2 columns and 1 line in from the sprite's corner */
    int bx = x0 - 2, by = y0 - 1;
    if (bx >= 0 && by >= 0 && bx + 16 <= 256 && by + 16 <= 192) {
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                uint32_t c = o[(by + y) * 256 + bx + x];
                /* only the bracket's L (its top lines, its left columns, the bend), not a ball beside it */
                corner[y][x] = (y <= 2 || x <= 4 || x + y <= 12) && is_cursor(c) ? c : 0;
            }
        have_corner = 1;
    }
    *bx0 = x0; *by0 = y0; *bx1 = x1; *by1 = y1;
    return 1;
}

/* How far the brackets stand out from their narrowest, around place k */
static int pulse(int k, int w) {
    if (!cursor_min_w[k] || w < cursor_min_w[k]) cursor_min_w[k] = w;
    return (w - cursor_min_w[k]) / 2;
}

static void draw_brackets(int x0, int y0, int x1, int y1) {
    if (!have_corner) return;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            uint32_t c = corner[y][x];
            if (!c) continue;
            put(x0 + x, y0 + y, c);
            put(x1 - x, y0 + y, c);
            put(x0 + x, y1 - y, c);
            put(x1 - x, y1 - y, c);
        }
}

static int bright_colour(uint32_t c) {                 /* a button's own colour: bright and saturated */
    int r = c >> 16 & 255, g = c >> 8 & 255, b = c & 255;
    int hi = r > g ? (r > b ? r : b) : (g > b ? g : b), lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
    return (c >> 24) && hi > 150 && hi - lo > 80;
}
static int light_colour(uint32_t c) {                  /* the Japanese label's white and its grey shadow */
    int r = c >> 16 & 255, g = c >> 8 & 255, b = c & 255;
    int hi = r > g ? (r > b ? r : b) : (g > b ? g : b), lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
    return (c >> 24) && hi > 130 && hi - lo < 40;
}
static uint32_t shade(uint32_t c, int num) {           /* c times num / 16 */
    return (c >> 24) << 24 | ((c >> 16 & 255) * num / 16) << 16 | ((c >> 8 & 255) * num / 16) << 8 | (c & 255) * num / 16;
}

/* A rectangle's pixels of one kind, copied centred on (cx, cy) */
static void copy_kind(int n, int x0, int y0, int w, int h, int (*kind)(uint32_t), int cx, int cy) {
    const uint32_t *l = layer(n);
    int a = 256, b = -1, t = 192, u = -1;
    for (int yy = y0; yy < y0 + h; yy++)
        for (int x = x0; x < x0 + w; x++)
            if (kind(l[yy * 256 + x])) {
                if (x < a) a = x;
                if (x > b) b = x;
                if (yy < t) t = yy;
                if (yy > u) u = yy;
            }
    if (b < 0) return;
    for (int yy = t; yy <= u; yy++)
        for (int x = a; x <= b; x++) {
            uint32_t c = l[yy * 256 + x];
            if (kind(c)) put(cx - (b - a + 1) / 2 + x - a, cy - (u - t + 1) / 2 + yy - t, c);
        }
}

/* One button: the tab (rows 0-10) with the Japanese word, the black bar (12-28) with the English word between
 * chevrons, the dotted body (29-41). accent: the button's colour. */
static void button(int b, int dx, int dy) {
    const uint32_t *l = layer(b == B_FIGHT ? 1 : 0);
    int sx = from[b].sx, sy = from[b].sy, sw = from[b].w;
    /* the colour: the bar's top line, or FIGHT's banner edge */
    uint32_t accent = 0;
    if (b == B_FIGHT) accent = l[88 * 256 + 100];
    else
        for (int y = sy; y < sy + from[b].h && !accent; y++)
            if (bright_colour(l[y * 256 + sx + sw / 2])) accent = l[y * 256 + sx + sw / 2];
    if (!bright_colour(accent)) return;
    uint32_t mid = shade(accent, 9), dark = shade(accent, 6), black = 0xff212121, dot0 = 0xff393939, dot1 = 0xff292929;
    if (!fade) return;
    for (int y = 0; y < BH; y++)
        for (int x = 0; x < BW; x++) {
            uint32_t c = 0;
            int dot = y % 3 == 2 ? dot0 : x % 3 ? dot1 : dot0;
            int m = y <= 20 ? y : BH - 1 - y;                          /* the shape is its own mirror */
            if (m <= 10) {                                             /* the tab, and its mirror below the bar */
                int in = 20 - m;
                if (x < in || x >= BW - in) continue;
                c = m == 0 || x == in || x == BW - 1 - in ? mid : x == in + 1 || x == BW - 2 - in ? dark : dot;
            } else if (m == 11) {                                      /* the tab's foot: within the bar's top */
                if (x < 6 || x >= BW - 6) continue;
                c = mid;
            } else {                                                   /* the bar, pointed at both ends */
                int in = (y < 20 ? 20 - y : y - 20) * 2 / 3;
                if (x < in || x >= BW - in) continue;
                c = m == 12 || x == in || x == BW - 1 - in ? accent : black;
                if (c == black && (x == in + 3 || x == BW - 4 - in) && y > 14 && y < 26) c = accent;   /* chevrons */
            }
            put(dx + x, dy + y, own(c & 0xffffff));
        }
    /* the words, as the game wrote them */
    if (b == B_FIGHT) {
        copy_kind(1, 112, 77, 31, 10, light_colour, dx + BW / 2, dy + 6);           /* たたかう */
        copy_kind(1, 101, 90, 54, 13, bright_colour, dx + BW / 2, dy + 20);         /* FIGHT! without its chevrons */
    } else {
        int bar = sy;                                                  /* the source's bar: its top line */
        while (bar < sy + from[b].h && !bright_colour(l[bar * 256 + sx + sw / 2])) bar++;
        int w0 = b == B_POKEMON ? sx + 12 : b == B_RUN ? sx + 10 : sx + 6, w1 = b == B_POKEMON ? sx + sw - 4 : b == B_RUN ? sx + sw - 10 : sx + sw - 14;
        copy_kind(0, sx + 8, sy, sw - 16, bar - sy - 1, light_colour, dx + BW / 2, dy + 6);
        copy_kind(0, w0, bar + 3, w1 - w0, 10, bright_colour, dx + BW / 2, dy + 20);     /* inside the bar's lines */
    }
}

/* Where each button is this frame: the selected one eases up to GROWN, the others back to 1, and the row stays
 * centred, so the others move aside */
static void row_layout(int sel, int *bx, int *by, int *bw, int *bh) {
    static float size[4] = {1, 1, 1, 1};
    static uint64_t last;
    int steps = rc_frames - last > 8 ? 8 : (int)(rc_frames - last);
    last = rc_frames;
    for (int b = 0; b < 4; b++)
        for (int k = 0; k < steps; k++) size[b] += ((b == sel ? GROWN : 1) - size[b]) * 0.3f;
    int total = 3 * GAP;
    for (int b = 0; b < 4; b++) {
        bw[b] = (int)(BW * size[b] + 0.5f);
        bh[b] = (int)(BH * size[b] + 0.5f);
        total += bw[b];
    }
    static const int order[4] = {B_FIGHT, B_POKEMON, B_BAG, B_RUN};
    int x = 128 - total / 2;
    for (int i = 0; i < 4; i++) {
        int b = order[i];
        bx[b] = x;
        by[b] = ROW_Y + BH / 2 - bh[b] / 2;
        x += bw[b] + GAP;
    }
}

/* ------------------------------------------------------------------ party balls */

/* The balls in a band of the touch screen's sprites, left to right, drawn 8 x 8 from (dx, dy) every 12 columns
 * (a bigger ball, the one in battle, shrunk to the same size) */
static void balls(int x0, int x1, int y0, int y1, int dx, int dy) {
    const uint32_t *o = layer(4);
    int x = x0, k = 0;
    while (x < x1) {
        int col = 0;
        for (int y = y0; y < y1 && !col; y++) col = o[y * 256 + x] && !in_cursor(x, y);
        if (!col) { x++; continue; }
        int a = x, b = x, t = y1, u = y0;
        for (; b < x1; b++) {
            int any = 0;
            for (int y = y0; y < y1; y++)
                if (o[y * 256 + b] && !in_cursor(b, y)) { any = 1; if (y < t) t = y; if (y > u) u = y; }
            if (!any) break;
        }
        int bw = b - a, bh = u - t + 1, bright = 0;
        for (int y = t; y <= u; y++)                             /* a ball has white or red in it */
            for (int xx = a; xx < b; xx++) {
                uint32_t c = o[y * 256 + xx];
                if (c && !in_cursor(xx, y) && ((c >> 16 & 255) > 150)) bright = 1;
            }
        if (!bright || bw < 5 || bh < 5) { x = b; continue; }
        for (int yy = 0; yy < 8; yy++)
            for (int xx = 0; xx < 8; xx++) {
                /* box filter over the part of the ball this pixel covers */
                int sx0 = a + xx * bw / 8, sx1 = a + (xx + 1) * bw / 8, sy0 = t + yy * bh / 8, sy1 = t + (yy + 1) * bh / 8;
                if (sx1 <= sx0) sx1 = sx0 + 1;
                if (sy1 <= sy0) sy1 = sy0 + 1;
                uint32_t r = 0, g = 0, bl = 0, n = 0, all = 0;
                for (int sy = sy0; sy < sy1; sy++)
                    for (int sx = sx0; sx < sx1; sx++) {
                        uint32_t c = o[sy * 256 + sx];
                        all++;
                        if (!c || in_cursor(sx, sy)) continue;
                        r += c >> 16 & 255; g += c >> 8 & 255; bl += c & 255; n++;
                    }
                if (n * 2 >= all && fade) put(dx + k * 12 + xx, dy + yy, own((r / n) << 16 | (g / n) << 8 | bl / n));
            }
        k++;
        x = b;
    }
}

/* ------------------------------------------------------------------ battle moves */

/* The game's four move buttons are 128 x 49 from (0, 31): the name on BG2 11 lines down, the PP line 28 down with the
 * type's tag (a sprite) at its left */
#define MOVE_X(c) ((c) * 128)
#define MOVE_Y(r) (31 + (r) * 49)
#define PLATE_W 168
#define PLATE_H 21
static const struct { int x, y; } plate_at[2][2] = {{{-41, 149}, {130, 149}}, {{-41, 170}, {130, 170}}};

/* The columns a layer drew in, in a rectangle: 0 when nothing */
static int drawn_box(int n, int x0, int y0, int w, int h, int *bx0, int *bx1) {
    const uint32_t *l = layer(n);
    *bx0 = 256; *bx1 = -1;
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            if (l[y * 256 + x]) { if (x < *bx0) *bx0 = x; if (x > *bx1) *bx1 = x; }
    return *bx1 >= 0;
}

static void plate(int dx, int dy, int w, int h, int empty) {
    const int cut = 4;
    for (int y = 0; y < h; y++) {
        int in = y < cut ? cut - y : y >= h - cut ? y - (h - cut) + 1 : 0;
        for (int x = in; x < w - in; x++) {
            int edge = y == 0 || y == h - 1 || x == in || x == w - 1 - in;
            if (fade) put(dx + x, dy + y, own(edge ? (empty ? 0x636363 : 0xf7f7ff) : empty ? 0x292929 : 0x212121));
        }
    }
}

static void battle_moves(void) {
    for (int r = 0; r < 2; r++)
        for (int c = 0; c < 2; c++) {
            int bx = MOVE_X(c), by = MOVE_Y(r), dx = plate_at[r][c].x, dy = plate_at[r][c].y, n0, n1, p0, p1;
            int named = drawn_box(2, bx + 4, by + 11, 120, 13, &n0, &n1);
            plate(dx, dy, PLATE_W, PLATE_H, !named);
            if (!named) continue;
            copy_layers(L_OBJ, bx + 17, by + 27, 30, 12, dx + 4, dy + 5);                 /* the type */
            copy_layers(L_BG2, n0, by + 11, n1 - n0 + 1, 13, dx + 38, dy + 4);           /* the name */
            if (drawn_box(2, bx + 48, by + 28, 76, 12, &p0, &p1)) {                        /* PP: the numbers */
                const uint32_t *l = layer(2);
                for (int x = p0, gap = 0; x <= p1; x++) {        /* past "PP" and the space after it */
                    int any = 0;
                    for (int y = by + 28; y < by + 40; y++) any |= l[y * 256 + x] != 0;
                    if (!any) gap++;
                    else if (gap >= 3) { p0 = x; break; }
                    else gap = 0;
                }
                copy_layers(L_BG2, p0, by + 28, p1 - p0 + 1, 12, dx + PLATE_W - 5 - (p1 - p0 + 1), dy + 5);
            }
        }
    int x0, y0, x1, y1;
    if (cursor_box(&x0, &y0, &x1, &y1)) {
        int c = (x0 + x1) / 2 >= 128, r = (y0 + y1) / 2 >= 80, p = pulse(4 + r * 2 + c, x1 - x0) + 2;
        draw_brackets(plate_at[r][c].x - p, plate_at[r][c].y - p, plate_at[r][c].x + PLATE_W - 1 + p,
                      plate_at[r][c].y + PLATE_H - 1 + p);
    }
}

/* ------------------------------------------------------------------ each frame */

#define BATTLE_SCALE 0.8f
#define BATTLE_RAISE 20
#define ENEMY_RAISE 14                  /* its box: 30-51 */
#define PLAYER_RAISE 24                 /* yours: 111-138 */

static int battle_command(void) { return active && rc_scene(SCENE_BATTLE_COMMAND); }

/* Before a frame is drawn: what the renderer keeps and leaves out */
void top_screen_prepare(int on) {
    active = on;
    render_keep_b = on;
    /* the battle: the view further back and raised, the HP boxes raised with it */
    int battle = on && rc_scene(SCENE_BATTLE);
    gpu3d_view_scale = battle ? BATTLE_SCALE : 1;
    gpu3d_view_raise = battle ? BATTLE_RAISE : 0;
    render_moves[0] = battle ? (RenderMove){-OFS, 128, 20, 58, -ENEMY_RAISE} : (RenderMove){0};
    render_moves[1] = battle ? (RenderMove){128, 256 + OFS, 100, 146, -PLAYER_RAISE} : (RenderMove){0};
    render_ui_mask = L_BG0 | L_BG1 | L_BG2 | L_OBJ;              /* all but the backgrounds on BG3 */
    /* the text box: the buttons go there, and the bag and the party go over it */
    render_hide[0] = active && (rc_scene(SCENE_BATTLE_COMMAND) || rc_scene(SCENE_BATTLE_MOVES) ||
                                rc_scene(SCENE_BATTLE_BAG) || rc_scene(SCENE_BATTLE_PARTY) ||
                                rc_scene(SCENE_FIELD_MENU) || rc_scene(SCENE_REPORT)) ? L_BG1 : 0;    /* (the field's: a band pointing down) */
}

/* After: the top screen as drawn, with the touch screen's parts on it */
void top_screen_compose(uint32_t *top_screen) {
    if (!active || !(io16(0x304) & 0x8000)) return;        /* engine A on the top screen, as in the field and battles */
    top = top_screen;
    uint16_t mb = io16(0x106c);
    int mf = (mb & 31) > 16 ? 16 : mb & 31;
    fade = mb >> 14 == 2 ? 255u * (uint32_t)(16 - mf) / 16 : 255;
    if (rc_scene(SCENE_FIELD_MENU)) {
        /* BG0 has the status bar on top and a bar along the bottom, and between them the selected button's label */
        if (background_up()) {
            copy_composed(16, 172);
            copy_layers(L_OBJ, 0, 172, 256, 20, 0, 172);          /* the close button, without the bar it's on */
        }
    } else if (rc_scene(SCENE_BATTLE_BAG) || rc_scene(SCENE_BATTLE_PARTY)) {
        if (background_up()) {
            /* the battle behind dimmed a little, with the fade's alpha, then the screen over it */
            uint32_t k = 256 - 96 * fade / 255;
            for (int i = 0; i < W * 192; i++) {
                uint32_t c = top[i];
                top[i] = ((c >> 16 & 255) * k >> 8) << 16 | ((c >> 8 & 255) * k >> 8) << 8 | (c & 255) * k >> 8;
            }
            copy_composed(0, 192);
        }
    } else if (rc_scene(SCENE_BATTLE_MOVES)) {
        battle_moves();                                          /* finds the cursor first */
        balls(40, 216, 20, 46, 10, 53 - ENEMY_RAISE);
        balls(40, 216, 120, 143, 170, 140 - PLAYER_RAISE);
    } else if (rc_scene(SCENE_BATTLE_COMMAND)) {
        int x0, y0, x1, y1, p = 0;
        game_cursor = -1;
        if (cursor_box(&x0, &y0, &x1, &y1)) {
            int cx = (x0 + x1) / 2;
            game_cursor = y1 - y0 > 70 ? B_FIGHT : cx < 88 ? B_BAG : cx < 168 ? B_RUN : B_POKEMON;
            p = pulse(game_cursor, x1 - x0);
        }
        int sel = game_cursor < 0 ? -1 : row_at >= 0 ? row_at : game_cursor;
        int bx[4], by[4], bw[4], bh[4];
        row_layout(sel, bx, by, bw, bh);
        for (int b = 0; b < 4; b++) {
            static uint32_t cell[BH][BW];
            memset(cell, 0, sizeof cell);
            canvas = &cell[0][0]; canvas_w = BW; canvas_h = BH;
            button(b, 0, 0);
            canvas = NULL;
            /* nearest pixel, scaled to its size this frame */
            for (int y = 0; y < bh[b]; y++)
                for (int x = 0; x < bw[b]; x++) {
                    uint32_t c = cell[y * BH / bh[b]][x * BW / bw[b]];
                    if (c) put(bx[b] + x, by[b] + y, c == 0xff000000u ? 0 : c);
                }
        }
        if (game_cursor >= 0) {
            int g = sel;
            p += 2;
            draw_brackets(bx[g] - p, by[g] - p, bx[g] + bw[g] - 1 + p, by[g] + bh[g] - 1 + p);
        }
        balls(40, 216, 20, 46, 10, 53 - ENEMY_RAISE);                          /* the opponent's, in a trainer battle */
        balls(40, 216, 120, 143, 170, 140 - PLAYER_RAISE);                      /* yours */
    }
}

/* Keys on the way to the game: in the battle commands, Left and Right move along our row (round from RUN to FIGHT and
 * back), turned into the presses that move the game's cursor between the same buttons. Some pairs aren't neighbours
 * in the game's own layout and take two presses; the second one's cursor sound is hushed. Presses are queued, and
 * where the cursor is going is kept here rather than read back from the game, which shows it a frame or more later.
 * Up and Down do nothing there. */
uint64_t snd_hush_cursor_until;          /* snd.c: no cursor sound until this frame */

void top_screen_keys(uint16_t *keys) {
    static uint16_t last = 0x3ff, hidden;
    static int queue[16], hush[16], queued, step, differ;
    uint16_t raw = *keys, pressed = last & ~raw;
    last = raw;
    if (!battle_command()) { hidden = 0; queued = step = 0; row_at = -1; return; }
    /* the game's cursor shows up on its own place (the last command chosen), or ends somewhere we didn't expect */
    if (row_at < 0 || (!queued && game_cursor >= 0 && game_cursor != row_at && ++differ > 10)) row_at = game_cursor;
    if (queued || game_cursor == row_at) differ = 0;
    if (pressed & (K_LEFT | K_RIGHT | K_UP | K_DOWN)) {
        if (game_cursor < 0 || row_at < 0) {
            /* the first press only shows the game's cursor: let it through */
        } else {
            hidden |= pressed & (K_LEFT | K_RIGHT | K_UP | K_DOWN);
            static const int order[4] = {B_FIGHT, B_POKEMON, B_BAG, B_RUN};
            int dir = pressed & K_RIGHT ? 1 : pressed & K_LEFT ? -1 : 0;
            if (dir) {
                int c = row_at, t = order[(slot[c] + dir + 4) % 4], k0 = 0, k1 = 0;
                /* the game's own moves between them */
                if (c == B_FIGHT && t == B_POKEMON) k0 = K_RIGHT;
                else if (c == B_POKEMON && t == B_FIGHT) k0 = K_UP;
                else if (c == B_POKEMON && t == B_BAG) k0 = k1 = K_LEFT;            /* through RUN */
                else if (c == B_BAG && t == B_POKEMON) k0 = k1 = K_RIGHT;           /* through RUN */
                else if (c == B_BAG && t == B_RUN) k0 = K_RIGHT;
                else if (c == B_RUN && t == B_BAG) k0 = K_LEFT;
                else if (c == B_RUN && t == B_FIGHT) k0 = K_UP;
                else if (c == B_FIGHT && t == B_RUN) { k0 = K_LEFT; k1 = K_RIGHT; }  /* through BAG (Down goes back to
                                                                                      * whichever it came from) */
                int n = queued - step / 6 > 0 ? queued : 0;          /* what's left of the queue, kept */
                if (!n) { queued = step = 0; }
                if (k0 && queued < 15) { queue[queued] = k0; hush[queued++] = 0; }
                if (k1 && queued < 16) { queue[queued] = k1; hush[queued++] = 1; }
                row_at = t;
            }
        }
    }
    hidden &= (uint16_t)~raw;            /* until let go */
    *keys |= hidden;
    if (queued) {                        /* each press held 2 frames, then 4 let go */
        int k = step / 6, f = step % 6;
        if (f == 0 && hush[k]) snd_hush_cursor_until = rc_frames + 6;
        if (f < 2) *keys &= (uint16_t)~queue[k];
        if (++step == queued * 6) queued = step = 0;
    }
}
