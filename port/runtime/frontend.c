/* Windows frontend: a window with both screens, keyboard and mouse input, 60 Hz pacing.
 *
 * Keys: arrows = D-pad, X = A, Z = B, S = X, A = Y, Q = L, W = R, Enter = Start, Backspace = Select,
 *       mouse on the lower screen = stylus, Tab (held) = fast forward, Esc = quit.
 * B2W2_HEADLESS=1 runs without a window; B2W2_SHOT_EVERY=n saves shot_<frame>.png every n frames. */
#include "internal.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdlib.h>
#include <stdio.h>

#define SCALE 2

uint16_t fe_keys = 0x03ff, fe_extkeys = 0x2c00;
static HWND wnd;
static uint32_t fb[256 * 384];
static LARGE_INTEGER freq, next_frame;
static int touch_down, touch_x, touch_y, quitting;
static int shot_every;

void arm7_save_flush(void);

/* ------------------------------------------------------------------ screenshots */

static uint32_t crc_table[256];
static uint32_t crc32_update(uint32_t c, const uint8_t *p, size_t n) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c = ~c;
    while (n--) c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
    return ~c;
}

static void put32be(FILE *f, uint32_t v) {
    uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    fwrite(b, 1, 4, f);
}

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    put32be(f, len);
    fwrite(type, 1, 4, f);
    if (len) fwrite(data, 1, len, f);
    uint32_t c = crc32_update(0, (const uint8_t *)type, 4);
    if (len) c = crc32_update(c, data, len);
    put32be(f, c);
}

/* An uncompressed PNG: a zlib stream of stored blocks, one filter byte per row */
static void write_png(const char *path, const uint32_t *px, int w, int h) {
    size_t raw_len = (size_t)h * (w * 3 + 1);
    uint8_t *raw = malloc(raw_len), *z = malloc(raw_len + raw_len / 65535 * 5 + 16);
    size_t o = 0, zo = 0;
    for (int y = 0; y < h; y++) {
        raw[o++] = 0;
        for (int x = 0; x < w; x++) {
            uint32_t c = px[y * w + x];
            raw[o++] = (uint8_t)(c >> 16); raw[o++] = (uint8_t)(c >> 8); raw[o++] = (uint8_t)c;
        }
    }
    z[zo++] = 0x78; z[zo++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t i = 0; i < raw_len;) {
        size_t n = raw_len - i > 65535 ? 65535 : raw_len - i;
        z[zo++] = (uint8_t)(i + n == raw_len);
        z[zo++] = (uint8_t)n; z[zo++] = (uint8_t)(n >> 8); z[zo++] = (uint8_t)~n; z[zo++] = (uint8_t)(~n >> 8);
        memcpy(z + zo, raw + i, n); zo += n; i += n;
    }
    uint32_t ad = b << 16 | a;
    z[zo++] = (uint8_t)(ad >> 24); z[zo++] = (uint8_t)(ad >> 16); z[zo++] = (uint8_t)(ad >> 8); z[zo++] = (uint8_t)ad;
    FILE *f = fopen(path, "wb");
    if (f) {
        static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
        fwrite(sig, 1, 8, f);
        uint8_t ihdr[13] = {0, 0, (uint8_t)(w >> 8), (uint8_t)w, 0, 0, (uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0};
        chunk(f, "IHDR", ihdr, 13);
        chunk(f, "IDAT", z, (uint32_t)zo);
        chunk(f, "IEND", NULL, 0);
        fclose(f);
    }
    free(raw);
    free(z);
}

/* ------------------------------------------------------------------ window */

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE: case WM_DESTROY:
        quitting = 1;
        return 0;
    case WM_LBUTTONDOWN: case WM_MOUSEMOVE: case WM_LBUTTONUP: {
        /* The touch screen is always the lower half of the window */
        int x = (short)LOWORD(lp) / SCALE, y = (short)HIWORD(lp) / SCALE - 192;
        touch_down = (wp & MK_LBUTTON) && msg != WM_LBUTTONUP && x >= 0 && x < 256 && y >= 0 && y < 192;
        if (touch_down) { touch_x = x; touch_y = y; }
        if (msg == WM_LBUTTONDOWN) SetCapture(h);
        if (msg == WM_LBUTTONUP) ReleaseCapture();
        return 0;
    }
    }
    return DefWindowProcA(h, msg, wp, lp);
}

void fe_init(void) {
    const char *e = getenv("B2W2_SHOT_EVERY");
    shot_every = e ? atoi(e) : 0;
    if (getenv("B2W2_HEADLESS")) return;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "b2w2";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    RECT r = {0, 0, 256 * SCALE, 384 * SCALE};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME, FALSE);
    wnd = CreateWindowA("b2w2", "Pocket Monsters White 2", (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME) | WS_VISIBLE,
                        CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance,
                        NULL);
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&next_frame);
    timeBeginPeriod(1);
}

static int down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

/* Scripted input for testing: B2W2_INPUT="600:A,700:START,900:T128x96" holds a button (A B SELECT START RIGHT
 * LEFT UP DOWN R L X Y) or touches a point for 6 frames from the given frame */
static int scripted(uint16_t *keys, uint16_t *ext) {
    static const char *names[12] = {"A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L", "X", "Y"};
    const char *s = getenv("B2W2_INPUT");
    int any = 0;
    while (s && *s) {
        unsigned long long f = strtoull(s, (char **)&s, 10);
        if (*s++ != ':') break;
        char name[16] = {0};
        int n = 0;
        while (*s && *s != ',' && n < 15) name[n++] = *s++;
        if (*s == ',') s++;
        if (rc_frames < f || rc_frames >= f + 6) continue;
        any = 1;
        if (name[0] == 'T') { touch_down = 1; sscanf(name + 1, "%dx%d", &touch_x, &touch_y); continue; }
        for (int i = 0; i < 12; i++)
            if (!strcmp(name, names[i])) { if (i < 10) *keys &= ~(1u << i); else *ext &= ~(0x400u << (i - 10)); }
    }
    return any;
}

static void read_input(void) {
    int focus = wnd && GetForegroundWindow() == wnd;
    static const int map[10] = {'X', 'Z', VK_BACK, VK_RETURN, VK_RIGHT, VK_LEFT, VK_UP, VK_DOWN, 'W', 'Q'};
    uint16_t k = 0x03ff, script_ext = 0x2c00;
    if (!wnd) touch_down = 0;
    scripted(&k, &script_ext);
    for (int i = 0; i < 10; i++) if (focus && down(map[i])) k &= ~(1u << i);
    fe_keys = k;
    /* X, Y and Debug as the ARM7 leaves them at 0x02ffffa8 for the SDK's pad reading (func_0203d980): bits 10, 11
     * and 13, 0 = pressed, ORed into KEYINPUT; bit 15 means the lid is closed and blanks all keys */
    uint16_t ext = script_ext;
    if (focus && down('S')) ext &= ~0x0400u;
    if (focus && down('A')) ext &= ~0x0800u;
    fe_extkeys = ext;
    wr16(0x02ffffa8, ext);
    arm7_touch(touch_down, touch_x, touch_y);
    if (focus && down(VK_ESCAPE)) quitting = 1;
}

void fe_vblank(void) {
    int shot = shot_every > 0 && rc_frames % (uint64_t)shot_every == 0;
    if (wnd || shot) {
        /* POWCNT1 bit 15: engine A on the top screen, otherwise on the lower (touch) screen */
        uint32_t *a = fb, *b = fb + 256 * 192;
        if (!(io16(0x304) & 0x8000)) { a = fb + 256 * 192; b = fb; }
        render_frame(a, b);
    }
    if (shot) {
        char name[64];
        sprintf(name, "shot_%06llu.png", (unsigned long long)rc_frames);
        write_png(name, fb, 256, 384);
        if (getenv("B2W2_REGDUMP")) {
            rc_logf("regs @%llu: POWCNT %04x DISPCNT A %08x B %08x DISPCAP %08x VRAMCNT", (unsigned long long)rc_frames,
                    io16(0x304), io32(0), io32(0x1000), io32(0x64));
            for (int i = 0; i < 10; i++) if (i != 7) rc_logf(" %02x", rc_io[0x240 + i]);
            rc_logf("\n   A BGCNT %04x %04x %04x %04x  B BGCNT %04x %04x %04x %04x  3DCNT %04x MBRIGHT %04x %04x\n",
                    io16(8), io16(10), io16(12), io16(14), io16(0x1008), io16(0x100a), io16(0x100c), io16(0x100e),
                    io16(0x60), io16(0x6c), io16(0x106c));
        }
    }
    read_input();
    if (!wnd) return;

    HDC dc = GetDC(wnd);
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = 256;
    bi.bmiHeader.biHeight = -384;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    StretchDIBits(dc, 0, 0, 256 * SCALE, 384 * SCALE, 0, 0, 256, 384, fb, &bi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(wnd, dc);

    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
    if (quitting) {
        arm7_save_flush();
        ExitProcess(0);
    }

    /* 59.83 frames a second, unless Tab is held */
    LONGLONG period = freq.QuadPart * 560190 / RC_CLOCK;
    next_frame.QuadPart += period;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (down(VK_TAB) && GetForegroundWindow() == wnd) { next_frame = now; return; }
    if (now.QuadPart > next_frame.QuadPart + period * 4) next_frame = now;     /* fell behind: don't catch up */
    while (now.QuadPart < next_frame.QuadPart) {
        LONGLONG ms = (next_frame.QuadPart - now.QuadPart) * 1000 / freq.QuadPart;
        if (ms > 1) Sleep((DWORD)(ms - 1));
        QueryPerformanceCounter(&now);
    }
}
