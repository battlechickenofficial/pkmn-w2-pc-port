/* Save files. The save chip is one of the files in saves/ (a 512 KB .sav from this port or another emulator); the
 * one in use is named in saves/current.txt. The picker chooses which file to play (the title menu's extra item, or
 * F7), and which file a save goes to: the frontend opens it when the game starts writing a save, and the
 * save chip's whole image is then written to the chosen file. Beside each file, <name>.png shows the top screen from
 * when it was last saved. A save made before there were save files (b2w2.sav) becomes "Save 1". */
#include "saves.h"
#include "font.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAVE_DIR "saves"
#define MAX_SAVES 64

static char current[64];

static int exists(const char *p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }
static void path_of(char *out, const char *name, const char *ext) { sprintf(out, SAVE_DIR "\\%s%s", name, ext); }

static void write_current(void) {
    FILE *f = fopen(SAVE_DIR "\\current.txt", "w");
    if (f) { fputs(current, f); fclose(f); }
}

void saves_init(void) {
    CreateDirectoryA(SAVE_DIR, NULL);
    FILE *f = fopen(SAVE_DIR "\\current.txt", "r");
    if (f) {
        if (fgets(current, sizeof current, f)) current[strcspn(current, "\r\n")] = 0;
        fclose(f);
    }
    if (current[0]) return;
    strcpy(current, "Save 1");
    char p[MAX_PATH];
    path_of(p, current, ".sav");
    if (!exists(p) && exists("b2w2.sav")) CopyFileA("b2w2.sav", p, TRUE);
    write_current();
}

const char *saves_current_name(void) { return current; }

const char *saves_current_path(void) {
    static char p[MAX_PATH];
    path_of(p, current, ".sav");
    return p;
}

void saves_set_current(const char *name) {
    if (!strcmp(name, current)) return;
    strncpy(current, name, sizeof current - 1);
    write_current();
}

void saves_picture_path(char *out, const char *name) { path_of(out, name, ".png"); }
void saves_panel_path(char *out, const char *name) { path_of(out, name, ".report.png"); }

/* ------------------------------------------------------------------ what a file holds */

typedef struct {
    char name[64];
    int found;                 /* the file exists (the file in use may not have been saved yet) */
    int foreign;               /* another game's save: White 2 can't load it */
    const char *game;
    wchar_t trainer[9];
    int hours, minutes;
    char when[32];             /* last saved, local time */
    uint32_t *pic;             /* the top screen at that time, or NULL */
    int pw, ph;
    uint32_t *panel;           /* the game's report panel at that time (date, place, party, badges, Pokedex, play
                                * time), or NULL */
    int qw, qh;
} SaveInfo;

/* "Save 2" before "Save 10" */
static int name_cmp(const void *pa, const void *pb) {
    const char *a = ((const SaveInfo *)pa)->name, *b = ((const SaveInfo *)pb)->name;
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            char *ea, *eb;
            unsigned long x = strtoul(a, &ea, 10), y = strtoul(b, &eb, 10);
            if (x != y) return x < y ? -1 : 1;
            a = ea; b = eb;
            continue;
        }
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* Our own PNGs only (frontend.c writes them uncompressed: stored deflate blocks, filter 0) */
static uint32_t *load_png(const char *path, int *pw, int *ph) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n), *z = malloc((size_t)n);
    uint32_t *px = NULL;
    size_t zn = 0;
    int w = 0, h = 0;
    if (fread(b, 1, (size_t)n, f) != (size_t)n || n < 8 || memcmp(b, "\x89PNG", 4)) goto done;
    for (long o = 8; o + 12 <= n;) {
        uint32_t len = (uint32_t)b[o] << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3];
        const uint8_t *t = b + o + 4, *d = b + o + 8;
        if (o + 12 + (long)len > n) break;
        if (!memcmp(t, "IHDR", 4)) {
            w = d[2] << 8 | d[3]; h = d[6] << 8 | d[7];
            if (d[8] != 8 || d[9] != 2 || d[12] != 0) goto done;
        } else if (!memcmp(t, "IDAT", 4)) {
            memcpy(z + zn, d, len); zn += len;
        }
        o += 12 + len;
    }
    size_t raw_len = (size_t)h * (w * 3 + 1), ro = 0;
    if (!w || !h || zn < 2) goto done;
    uint8_t *raw = malloc(raw_len);
    for (size_t i = 2; i + 5 <= zn;) {
        int last = z[i] & 1, type = (z[i] >> 1) & 3;
        size_t len = z[i + 1] | z[i + 2] << 8;
        if (type || i + 5 + len > zn || ro + len > raw_len) break;
        memcpy(raw + ro, z + i + 5, len); ro += len; i += 5 + len;
        if (last) break;
    }
    if (ro == raw_len) {
        px = malloc((size_t)w * h * 4);
        for (int y = 0; y < h && px; y++) {
            const uint8_t *r = raw + (size_t)y * (w * 3 + 1);
            if (r[0]) { free(px); px = NULL; break; }
            for (int x = 0; x < w; x++) px[y * w + x] = (uint32_t)r[1 + x * 3] << 16 | r[2 + x * 3] << 8 | r[3 + x * 3];
        }
    }
    free(raw);
    *pw = w; *ph = h;
done:
    fclose(f);
    free(b);
    free(z);
    return px;
}

/* The trainer card block of a Black/White (2) save: name (UTF-16, 0xffff-terminated) at +4, play time at +0x24,
 * game version at +0x1f */
static void read_info(SaveInfo *s) {
    char p[MAX_PATH];
    path_of(p, s->name, ".sav");
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (GetFileAttributesExA(p, GetFileExInfoStandard, &a)) {
        FILETIME lt;
        SYSTEMTIME st;
        FileTimeToLocalFileTime(&a.ftLastWriteTime, &lt);
        FileTimeToSystemTime(&lt, &st);
        sprintf(s->when, "%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    }
    FILE *f = fopen(p, "rb");
    if (f) {
        static uint8_t b[0x19430];
        size_t got = fread(b, 1, sizeof b, f);
        fclose(f);
        s->found = 1;
        if (got == sizeof b) {
            const uint8_t *t = b + 0x19400;
            uint8_t v = t[0x1f];
            s->game = v == 0x16 ? "White 2" : v == 0x17 ? "Black 2" : v == 0x14 ? "White" : v == 0x15 ? "Black" : NULL;
            s->foreign = v == 0x14 || v == 0x15;
            for (int i = 0; i < 8; i++) {
                uint16_t c = (uint16_t)(t[4 + i * 2] | t[5 + i * 2] << 8);
                if (c == 0xffff || !c) break;
                s->trainer[i] = c;
            }
            if (s->game) { s->hours = t[0x24] | t[0x25] << 8; s->minutes = t[0x26]; }
        }
    }
    path_of(p, s->name, ".png");
    s->pic = load_png(p, &s->pw, &s->ph);
    path_of(p, s->name, ".report.png");
    s->panel = load_png(p, &s->qw, &s->qh);
}

static int list_saves(SaveInfo *out) {
    int n = 0, have_current = 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(SAVE_DIR "\\*.sav", &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            size_t len = strlen(fd.cFileName) - 4;
            if (n == MAX_SAVES || len >= sizeof out->name) continue;
            memset(&out[n], 0, sizeof out[n]);
            memcpy(out[n].name, fd.cFileName, len);
            have_current |= !_stricmp(out[n].name, current);
            n++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (!have_current && n < MAX_SAVES) { memset(&out[n], 0, sizeof out[n]); strcpy(out[n++].name, current); }
    qsort(out, (size_t)n, sizeof *out, name_cmp);
    for (int i = 0; i < n; i++) read_info(&out[i]);
    return n;
}

static void new_name(char *out) {
    for (int k = 1;; k++) {
        char p[MAX_PATH];
        sprintf(out, "Save %d", k);
        path_of(p, out, ".sav");
        if (!exists(p) && _stricmp(out, current)) return;
    }
}

/* ------------------------------------------------------------------ the picker */

/* Drawn on the top screen at the DS's resolution, in the title menu's style: striped background, the menu's buttons
 * (grey frame, dark face; cyan frame when selected), the game's font. The files are listed on the left, a picture
 * in each; on the right the selected file's report panel from when it was saved (date, place, party, badges,
 * Pokedex, play time), or on the new file while saving, the game being saved. Under it, the file's picture and
 * date. */

#define W 342                          /* buffer width; the DS's 256 columns start at OFS */
#define OFS 43
#define ROWS 4                         /* list rows */
#define ROW_Y(i) (22 + 38 * (i))
#define LIST_X0 (-40)
#define LIST_X1 98
#define TW 48                          /* list pictures */
#define TH 27
#define PANEL_X 102                    /* the report panel */
#define PANEL_Y 21
#define PANEL_W 196
#define PANEL_H 115

static struct {
    int open, mode, n, sel, top, result;
    SaveInfo s[MAX_SAVES];
    uint32_t small[MAX_SAVES][TW * TH];
    char chosen[64];
} pk;
static uint32_t current_panel[PANEL_W * PANEL_H];
static int current_qw, current_qh;

/* The report panel of the game in progress, shown on the new file while saving (NULL: none yet) */
void picker_set_current_panel(const uint32_t *px, int w, int h) {
    if (!px) { current_qw = current_qh = 0; return; }
    current_qw = w > PANEL_W ? PANEL_W : w;
    current_qh = h > PANEL_H ? PANEL_H : h;
    for (int y = 0; y < current_qh; y++) memcpy(current_panel + y * current_qw, px + y * w, current_qw * 4);
}

#define RGB24(r, g, b) ((uint32_t)(r) << 16 | (uint32_t)(g) << 8 | (uint32_t)(b))
static const uint32_t TEXT = RGB24(239, 239, 239), TEXT_SHADOW = RGB24(140, 140, 140);
static const uint32_t INK = RGB24(66, 66, 66), INK_SHADOW = RGB24(189, 189, 189);

/* A box-filtered copy of a picture */
static void shrink(const uint32_t *src, int sw, int sh, uint32_t *dst, int dw, int dh) {
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++) {
            int x0 = x * sw / dw, x1 = (x + 1) * sw / dw, y0 = y * sh / dh, y1 = (y + 1) * sh / dh;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    uint32_t c = src[yy * sw + xx];
                    r += c >> 16 & 255; g += c >> 8 & 255; b += c & 255; n++;
                }
            dst[y * dw + x] = n ? RGB24(r / n, g / n, b / n) : 0;
        }
}

void picker_open(int mode) {
    pk.open = 1;
    pk.mode = mode;
    pk.result = 0;
    pk.top = 0;
    pk.n = list_saves(pk.s);
    pk.sel = 0;
    for (int i = 0; i < pk.n; i++) {
        if (!_stricmp(pk.s[i].name, current)) pk.sel = i;
        if (pk.s[i].pic) shrink(pk.s[i].pic, pk.s[i].pw, pk.s[i].ph, pk.small[i], TW, TH);
    }
    if (pk.sel >= ROWS) pk.top = pk.sel - ROWS + 1;
}

int picker_active(void) { return pk.open; }

void picker_close(void) {
    for (int i = 0; i < pk.n; i++) {
        free(pk.s[i].pic); pk.s[i].pic = NULL;
        free(pk.s[i].panel); pk.s[i].panel = NULL;
    }
    pk.open = 0;
}

int picker_result(char name[64]) {
    if (pk.result == 1) strcpy(name, pk.chosen);
    return pk.result;
}

void snd_menu_sound(int k);             /* snd.c: 0 cursor, 1 decide, 2 cancel */

void picker_key(int button) {
    int items = pk.n + 1, was = pk.sel;
    switch (button) {
    case PK_UP: pk.sel = (pk.sel + items - 1) % items; break;            /* wraps, like the game's menus */
    case PK_DOWN: pk.sel = (pk.sel + 1) % items; break;
    case PK_PAGE_UP: pk.sel = pk.sel > ROWS ? pk.sel - ROWS : 0; break;
    case PK_PAGE_DOWN: pk.sel = pk.sel + ROWS < items ? pk.sel + ROWS : items - 1; break;
    case PK_A:
        if (pk.sel < pk.n && pk.s[pk.sel].foreign) break;            /* another game's: nothing happens */
        if (pk.sel < pk.n) strcpy(pk.chosen, pk.s[pk.sel].name);
        else new_name(pk.chosen);
        pk.result = 1;
        snd_menu_sound(1);
        break;
    case PK_B: pk.result = -1; snd_menu_sound(2); break;
    }
    if (pk.sel != was) snd_menu_sound(0);
    if (pk.sel < pk.top) pk.top = pk.sel;
    if (pk.sel >= pk.top + ROWS) pk.top = pk.sel - ROWS + 1;
}

/* ---- drawing into a screen buffer (x in DS columns: 0-255 is the DS picture, -43 to 298 with the margins) */

typedef struct { uint32_t *buf; uint32_t c1, c2; int clip0, clip1; } Pen;

static void px(uint32_t *buf, int x, int y, uint32_t c) {
    if (x >= -OFS && x < 256 + OFS && y >= 0 && y < 192) buf[y * W + x + OFS] = c;
}
static void rect(uint32_t *buf, int x0, int y0, int x1, int y1, uint32_t c) {
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) px(buf, x, y, c);
}
static void pen_put(void *ctx, int x, int y, int v) {
    Pen *p = ctx;
    if (x >= p->clip0 && x < p->clip1) px(p->buf, x, y, v == 1 ? p->c1 : p->c2);
}
static int say(uint32_t *buf, const wchar_t *s, int x, int y, int x_end, uint32_t c1, uint32_t c2) {
    Pen p = {buf, c1, c2, x, x_end};
    return font_text(s, x, y, pen_put, &p);
}

/* The title menu's background: grey-white lines */
static void stripes(uint32_t *buf) {
    for (int y = 0; y < 192; y++) rect(buf, -OFS, y, 256 + OFS, y + 1, y & 1 ? RGB24(231, 231, 231) : RGB24(255, 255, 255));
}

/* A button in the menu's style: three frame lines, a face with a lighter line just inside */
static void button(uint32_t *buf, int x0, int y0, int x1, int y1, int selected) {
    uint32_t edge = selected ? RGB24(82, 255, 255) : RGB24(140, 140, 140);
    uint32_t mid = selected ? RGB24(247, 255, 255) : RGB24(255, 255, 255);
    uint32_t face = selected ? RGB24(99, 107, 115) : RGB24(66, 66, 66);
    uint32_t line = selected ? RGB24(99, 115, 132) : RGB24(82, 82, 82);
    rect(buf, x0, y0, x1, y1, edge);
    rect(buf, x0 + 1, y0 + 1, x1 - 1, y1 - 1, mid);
    rect(buf, x0 + 2, y0 + 2, x1 - 2, y1 - 2, edge);
    rect(buf, x0 + 3, y0 + 3, x1 - 3, y1 - 3, line);
    rect(buf, x0 + 4, y0 + 4, x1 - 4, y1 - 4, face);
}

static void picture(uint32_t *buf, const uint32_t *pic, int pw, int ph, int x0, int y0) {
    for (int y = 0; y < ph; y++) for (int x = 0; x < pw; x++) px(buf, x0 + x, y0 + y, pic[y * pw + x]);
}

static void display_name(const SaveInfo *s, wchar_t *out) {
    int k;
    char rest;
    if (sscanf(s->name, "Save %d%c", &k, &rest) == 1) swprintf(out, 64, L"レポート %d", k);
    else MultiByteToWideChar(CP_ACP, 0, s->name, -1, out, 64);
}

static const wchar_t *game_name(const char *g) {
    return !g ? L"" : !strcmp(g, "White 2") ? L"ポケモン ホワイト２" : !strcmp(g, "Black 2") ? L"ポケモン ブラック２" :
           !strcmp(g, "White") ? L"ポケモン ホワイト" : L"ポケモン ブラック";
}

/* What a file holds, in a line */
static void summary(const SaveInfo *s, wchar_t *out) {
    if (s->foreign) swprintf(out, 64, L"つかえません");
    else if (s->trainer[0]) swprintf(out, 64, L"%ls", s->trainer);
    else swprintf(out, 64, s->found ? L"レポートなし" : L"からっぽ");
}

/* The right column: a report panel, or for a file saved before panels were kept, what the file itself says */
static void panel(uint32_t *top, const SaveInfo *s) {
    rect(top, PANEL_X, PANEL_Y, PANEL_X + PANEL_W, PANEL_Y + PANEL_H, RGB24(140, 140, 140));
    rect(top, PANEL_X + 1, PANEL_Y + 1, PANEL_X + PANEL_W - 1, PANEL_Y + PANEL_H - 1, RGB24(66, 66, 66));
    const uint32_t *q = NULL;
    int qw = 0, qh = 0;
    if (s && s->panel) { q = s->panel; qw = s->qw; qh = s->qh; }
    else if (!s && pk.mode == PICK_SAVE && current_qw) { q = current_panel; qw = current_qw; qh = current_qh; }
    if (q) {
        for (int y = 0; y < qh && y < PANEL_H - 2; y++)
            for (int x = 0; x < qw && x < PANEL_W - 2; x++) px(top, PANEL_X + 1 + x, PANEL_Y + 1 + y, q[y * qw + x]);
        return;
    }
    wchar_t line[96];
    int y = PANEL_Y + 8;
    if (!s) {
        say(top, pk.mode == PICK_SAVE ? L"あたらしい レポートに" : L"さいしょから", PANEL_X + 8, y, 296, TEXT, TEXT_SHADOW);
        say(top, pk.mode == PICK_SAVE ? L"かきこみます" : L"はじめます", PANEL_X + 8, y + 18, 296, TEXT, TEXT_SHADOW);
        return;
    }
    if (s->foreign) {
        say(top, game_name(s->game), PANEL_X + 8, y, 296, TEXT, TEXT_SHADOW);
        say(top, L"このゲームでは", PANEL_X + 8, y + 18, 296, RGB24(255, 156, 156), TEXT_SHADOW);
        say(top, L"つかえません", PANEL_X + 8, y + 36, 296, RGB24(255, 156, 156), TEXT_SHADOW);
        return;
    }
    if (!s->trainer[0]) { say(top, s->found ? L"まだ レポートが ありません" : L"からっぽ", PANEL_X + 8, y, 296, TEXT, TEXT_SHADOW); return; }
    say(top, s->trainer, PANEL_X + 8, y, 296, TEXT, TEXT_SHADOW);
    say(top, game_name(s->game), PANEL_X + 8, y + 18, 296, TEXT, TEXT_SHADOW);
    swprintf(line, 96, L"プレイじかん %d:%02d", s->hours, s->minutes);
    say(top, line, PANEL_X + 8, y + 36, 296, TEXT, TEXT_SHADOW);
}

void picker_render(uint32_t *top, uint32_t *bottom) {
    (void)bottom;
    int items = pk.n + 1;
    stripes(top);

    /* the title bar, and which buttons do what along the bottom */
    rect(top, -OFS, 0, 256 + OFS, 18, RGB24(66, 66, 66));
    rect(top, -OFS, 18, 256 + OFS, 19, RGB24(140, 140, 140));
    say(top, pk.mode == PICK_SAVE ? L"どの レポートに かきこみますか？" : L"レポートを えらんでください", -36, 2, 290, TEXT,
        TEXT_SHADOW);
    if (pk.top > 0)
        for (int k = 0; k < 4; k++) rect(top, 286 - k, 5 + k, 287 + k, 6 + k, TEXT);
    if (pk.top + ROWS < items)
        for (int k = 0; k < 4; k++) rect(top, 286 - k, 13 - k, 287 + k, 14 - k, TEXT);
    rect(top, -OFS, 175, 256 + OFS, 176, RGB24(140, 140, 140));
    rect(top, -OFS, 176, 256 + OFS, 192, RGB24(66, 66, 66));
    say(top, pk.mode == PICK_SAVE ? L"Ａ かきこむ　　Ｂ やめる" : L"Ａ あそぶ　　Ｂ もどる", -36, 177, 290, TEXT, TEXT_SHADOW);

    /* the list */
    for (int r = 0; r < ROWS && pk.top + r < items; r++) {
        int i = pk.top + r, y = ROW_Y(r);
        const SaveInfo *s = i < pk.n ? &pk.s[i] : NULL;
        button(top, LIST_X0, y, LIST_X1, y + 36, i == pk.sel);
        int tx = LIST_X0 + 4, ty = y + 4;
        rect(top, tx, ty, tx + TW, ty + TH + 1, RGB24(33, 33, 33));
        if (s && s->pic) picture(top, pk.small[i], TW, TH, tx, ty + 1);
        else if (!s) { rect(top, tx + TW / 2 - 1, ty + 7, tx + TW / 2 + 1, ty + TH - 5, TEXT); rect(top, tx + 14, ty + TH / 2, tx + TW - 14, ty + TH / 2 + 2, TEXT); }
        wchar_t line[64];
        if (s) display_name(s, line);
        else swprintf(line, 64, L"あたらしく");
        say(top, line, tx + TW + 4, y + 3, LIST_X1 - 4, TEXT, TEXT_SHADOW);
        if (s) summary(s, line);
        else swprintf(line, 64, pk.mode == PICK_SAVE ? L"かきこむ" : L"はじめる");
        uint32_t c = s && !_stricmp(s->name, current) ? RGB24(140, 255, 255) : s && s->foreign ? RGB24(255, 156, 156) : TEXT;
        say(top, line, tx + TW + 4, y + 19, LIST_X1 - 4, c, TEXT_SHADOW);
    }

    /* the right column */
    const SaveInfo *s = pk.sel < pk.n ? &pk.s[pk.sel] : NULL;
    panel(top, s);
    int py = PANEL_Y + PANEL_H + 2;
    rect(top, PANEL_X, py, PANEL_X + 64, py + 37, RGB24(140, 140, 140));
    rect(top, PANEL_X + 1, py + 1, PANEL_X + 63, py + 36, RGB24(33, 33, 33));
    if (s && s->pic) {
        static uint32_t mid[62 * 35];
        static int mid_of = -1, mid_open;
        if (mid_of != pk.sel || mid_open != pk.open) { shrink(s->pic, s->pw, s->ph, mid, 62, 35); mid_of = pk.sel; mid_open = pk.open; }
        picture(top, mid, 62, 35, PANEL_X + 1, py + 1);
    }
    wchar_t line[96], nm[64];
    if (s) {
        display_name(s, nm);
        int end = say(top, nm, PANEL_X + 70, py + 2, 298, INK, INK_SHADOW);
        if (!_stricmp(s->name, current)) say(top, L"しようちゅう", end + 6, py + 2, 298, RGB24(33, 132, 140), INK_SHADOW);
        int y, mo, d, h, mi;
        if (sscanf(s->when, "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) == 5) {
            swprintf(line, 96, L"%d/%02d/%02d %d:%02d", y, mo, d, h, mi);
            say(top, line, PANEL_X + 70, py + 19, 298, INK, INK_SHADOW);
        }
    } else {
        say(top, L"あたらしい レポート", PANEL_X + 70, py + 2, 298, INK, INK_SHADOW);
    }
}
