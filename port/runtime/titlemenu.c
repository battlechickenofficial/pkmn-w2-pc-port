/* The title menu's extra item: the save files, second in the list, below "New Game" (さいしょから はじめる). The
 * game knows nothing of it. Its menu is one button every 24 lines from line 16 in screen columns 32-223: the frames on
 * BG2 (blended over the Poke Ball on BG3), the words on BG1, icons as sprites. While the menu is up, each line of
 * those layers is changed before it is composed: the buttons after "New Game" move down a step, and the gap gets a
 * button built from the game's own frame lines with its words in the game's font. Up and Down move onto it as onto
 * the others: the game's cursor stays on a neighbour, which is drawn unselected while ours is. A opens the picker.
 *
 * Everything is read off the layers: where the buttons are, which one is selected (its frame colour differs), and
 * which one is "New Game" (its words match the font). What one frame finds, the next frame uses. */
#include "render.h"
#include "font.h"
#include <stdlib.h>

#define MX0 32                          /* the menu's columns */
#define MX1 224
#define MW (MX1 - MX0)
#define TOP(k) (16 + 24 * (k))
#define MAXB 7                          /* buttons that fit with ours: the last ends on line 191 */

enum { K_A = 1, K_B = 2, K_UP = 0x40, K_DOWN = 0x80 };

typedef struct { uint16_t color[MW]; uint8_t prio[MW], flags[MW], alpha[MW]; } Strip;
static Strip cur[4][192], prev[4][192]; /* BG0, BG1, BG2, sprites as the game drew them, this frame and the last */

/* Found last frame. While the game moves its cursor, the highlight fades from one button to the next for a few
 * frames and no single button is selected; sel stays the last one that was. */
static int count, start = -1, sel = -1;
static uint16_t text_colour[2][3] = {{0, 0x77bd, 0x4631}, {0, 0x77bd, 0x4631}};   /* [selected][1 letter, 2 shadow] */
static int frame_ok;                    /* the menu is up and was understood */

static int ours, anchor;                /* our item is selected; the game's cursor is on anchor (start or start + 1) */
static int leaving;                     /* frames left in which the game moves its cursor off anchor, after ours */

void snd_menu_sound(int k);             /* snd.c: 0 cursor, 1 decide, 2 cancel */

static Line *layer_of(Line *bg, Line *obj, int i) { return i < 3 ? &bg[i] : obj; }

static void save_strip(Strip *s, const Line *l) {
    memcpy(s->color, l->color + AT(MX0), sizeof s->color);
    memcpy(s->prio, l->prio + AT(MX0), MW);
    memcpy(s->flags, l->flags + AT(MX0), MW);
    memcpy(s->alpha, l->alpha + AT(MX0), MW);
}

static void load_strip(Line *l, const Strip *s) {
    memcpy(l->color + AT(MX0), s->color, sizeof s->color);
    memcpy(l->prio + AT(MX0), s->prio, MW);
    memcpy(l->flags + AT(MX0), s->flags, MW);
    memcpy(l->alpha + AT(MX0), s->alpha, MW);
}

static int menu_up(void) { return rc_title_menu_frame && rc_frames <= rc_title_menu_frame + 2; }

/* ------------------------------------------------------------------ words */

/* A label's pixels, as the game places them on a button: from x 40, 4 lines below the button's top */
typedef struct { uint8_t v[FONT_H][MW]; } Mask;
static void mask_put(void *ctx, int x, int y, int v) {
    Mask *m = ctx;
    if (x >= MX0 && x < MX1 && y >= 0 && y < FONT_H) m->v[y][x - MX0] = (uint8_t)v;
}
static const Mask *label_mask(const wchar_t *s) {
    static struct { const wchar_t *s; Mask m; } cache[8];
    for (int i = 0; i < 8; i++) {
        if (cache[i].s == s) return &cache[i].m;
        if (!cache[i].s) { cache[i].s = s; memset(&cache[i].m, 0, sizeof cache[i].m); font_text(s, 40, 0, mask_put, &cache[i].m); return &cache[i].m; }
    }
    return NULL;
}

static const wchar_t *const known[] = {L"つづきから　はじめる", L"さいしょから　はじめる", L"ふしぎな　おくりもの",
                                       L"イッシュリンク", L"マイクテスト"};

/* Whether button k's words (last frame) are label s; if so, note their colours for that button's style */
static int words_are(int k, const wchar_t *s) {
    const Mask *m = label_mask(s);
    if (!m) return 0;
    uint16_t c[3] = {0};
    int seen[3] = {0};
    for (int y = 0; y < FONT_H; y++) {
        const Strip *st = &prev[1][TOP(k) + 4 + y];
        for (int x = 0; x < MW; x++) {
            int v = m->v[y][x], on = st->flags[x] & OPAQUE;
            if (!v != !on) return 0;
            if (!v) continue;
            if (seen[v] && c[v] != st->color[x]) return 0;
            seen[v] = 1; c[v] = st->color[x];
        }
    }
    if (seen[1] && seen[2]) { text_colour[k == sel][1] = c[1]; text_colour[k == sel][2] = c[2]; }
    return 1;
}

/* Our words on line y of BG1, for a button whose top is at line top */
typedef struct { Line *l; int row; uint8_t prio; const uint16_t *col; } Put;
static void line_put(void *ctx, int x, int y, int v) {
    Put *p = ctx;
    if (y != p->row || x < MX0 || x >= MX1 || v > 2) return;
    int i = AT(x);
    p->l->color[i] = p->col[v]; p->l->prio[i] = p->prio; p->l->flags[i] = OPAQUE; p->l->alpha[i] = 31;
}

/* Selected words drawn unselected */
static void unselect_words(Line *l) {
    for (int x = MX0; x < MX1; x++) {
        int i = AT(x);
        if (!(l->flags[i] & OPAQUE)) continue;
        if (l->color[i] == text_colour[1][1]) l->color[i] = text_colour[0][1];
        else if (l->color[i] == text_colour[1][2]) l->color[i] = text_colour[0][2];
    }
}

/* ------------------------------------------------------------------ each line */

/* What last frame's lines say: the buttons (a frame line one below each top), the selected one, "New Game" */
static void understand(void) {
    uint16_t c[MAXB];
    int n = 0;
    while (n < MAXB + 1 && TOP(n) + 23 < 192) {
        const Strip *s = &prev[2][TOP(n) + 1];
        if (!(s->flags[150 - MX0] & OPAQUE)) break;
        if (n < MAXB) c[n] = s->color[150 - MX0];
        n++;
    }
    if (n != count) sel = -1;
    count = n;
    start = -1;
    frame_ok = 0;
    if (n < 2 || n >= MAXB) return;
    int now = -1;
    for (int k = 0; k < n; k++) {
        int same = 0;
        for (int j = 0; j < n; j++) same += c[j] == c[k];
        if (same == 1) now = now == -1 ? k : -2;
    }
    if (now >= 0) sel = now;
    if (leaving && ((now >= 0 && now != anchor) || !--leaving)) leaving = 0;
    for (int k = 0; k < n; k++)
        for (int i = 0; i < (int)(sizeof known / sizeof *known); i++)
            if (words_are(k, known[i]) && i == 1) start = k;
    frame_ok = sel >= 0 && start >= 0 && start + 1 < n;
}

/* A button drawn unselected last frame, to copy frame lines from: none the highlight is on or moving between */
static int plain_button(void) {
    for (int k = count - 1; k >= 0; k--)
        if (k != anchor && k != start && k != start + 1 && (k < sel - 1 || k > sel + 1)) return k;
    return sel != start ? start : start + 1;
}

void title_menu_line(int y, Line *bg, Line *obj) {
    if (!menu_up()) { frame_ok = 0; ours = leaving = 0; return; }
    if (!font_ready()) return;
    for (int i = 0; i < 4; i++) save_strip(&cur[i][y], layer_of(bg, obj, i));
    if (y == 191) { memcpy(prev, cur, sizeof prev); understand(); }
    if (!frame_ok) return;

    int slot = start + 1, plain = plain_button();
    int hide_anchor = ours || leaving;                  /* the game's cursor is on anchor, but ours is (or was) selected */
    int k = -1, row = 0;                                 /* the game's button shown on this line, and its line in it */
    if (y >= TOP(slot) + 24 && y < TOP(count + 1)) {
        /* the game's buttons after "New Game", a step down */
        int from = y - 24;
        for (int i = 0; i < 4; i++) load_strip(layer_of(bg, obj, i), &cur[i][from]);
        k = (from - 16) / 24; row = from - TOP(k);
    } else if (y >= TOP(slot) && y < TOP(slot) + 24) {
        /* ours: the frame of a button in the style it needs, then its words */
        int r = y - TOP(slot), from = ours ? anchor : plain;
        load_strip(&bg[0], &prev[0][TOP(from) + r]);
        load_strip(&bg[2], &prev[2][TOP(from) + r]);
        for (int x = MX0; x < MX1; x++) { bg[1].flags[AT(x)] = 0; obj->flags[AT(x)] = 0; }
        Put p = {&bg[1], r - 4, (uint8_t)(io16(0x0a) & 3), text_colour[ours]};
        font_text(start == 0 ? L"つづきから　はじめる" : L"レポートを　えらぶ", 40, 0, line_put, &p);
        return;
    } else if (y >= 16 && y < TOP(slot)) {
        k = (y - 16) / 24; row = y - TOP(k);
    }
    if (k >= 0 && hide_anchor && k == anchor) {
        load_strip(&bg[2], &prev[2][TOP(plain) + row]);
        unselect_words(&bg[1]);
    }
}

/* ------------------------------------------------------------------ keys */

/* Called with the keys about to go to the game (KEYINPUT bits, 0 = pressed). Moves onto and off our item, hiding
 * from the game the presses it uses, with the menu's sounds where the game makes none. Returns 1 when A was pressed
 * on it. */
int title_menu_keys(uint16_t *keys) {
    static uint16_t last = 0x3ff, hidden;
    static int inject, inject_key;
    uint16_t raw = *keys, pressed = last & ~raw;
    last = raw;
    int open = 0;
    if (!frame_ok) { ours = 0; hidden = 0; inject = 0; return 0; }
    if (!ours) {
        if ((pressed & K_DOWN) && sel == start && !leaving) { ours = 1; anchor = start; hidden |= K_DOWN; snd_menu_sound(0); }
        else if ((pressed & K_UP) && sel == start + 1 && !leaving) { ours = 1; anchor = start + 1; hidden |= K_UP; snd_menu_sound(0); }
    } else if (pressed & (K_UP | K_DOWN)) {
        /* off ours: onto anchor (the game's cursor is there already) or past it (the game moves, and sounds) */
        int key = pressed & K_UP ? K_UP : K_DOWN;
        ours = 0; hidden |= key;
        if ((key == K_UP) == (anchor == start + 1)) { inject = 2; inject_key = key; leaving = 30; }
        else snd_menu_sound(0);
    } else if (pressed & K_A) {
        hidden |= K_A;
        snd_menu_sound(1);
        open = 1;
    } else if (pressed & K_B) {
        ours = 0;
    }
    hidden &= (uint16_t)~raw;            /* until let go */
    *keys |= hidden;
    if (ours) *keys |= K_UP | K_DOWN | K_A;
    if (inject) { *keys &= (uint16_t)~inject_key; inject--; }
    return open;
}
