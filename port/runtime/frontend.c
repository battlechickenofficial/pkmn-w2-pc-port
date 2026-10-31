/* Windows frontend: a resizable window or fullscreen with both screens (widescreen top screen), keyboard and mouse
 * input, sound, 60 Hz pacing.
 *
 * Keys: arrows = D-pad, X = A, Z = B, S = X, A = Y, Q = L, W = R, Enter = Start, Backspace = Select,
 *       mouse on the touch screen = stylus, Tab (held) = fast forward, F8 = speed 1x/2x/4x/8x, F11 or Alt+Enter = fullscreen,
 *       F10 = layout (stacked, big top screen, side by side, top screen only), F9 = widescreen on/off, Esc = leave fullscreen / quit.
 * B2W2_HEADLESS=1 runs without a window; B2W2_SHOT_EVERY=n saves shot_<frame>.png every n frames. */
#include "internal.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdlib.h>
#include <stdio.h>

uint16_t fe_keys = 0x03ff, fe_extkeys = 0x2c00;
static HWND wnd;
static LARGE_INTEGER freq, next_frame;
static int touch_down, touch_x, touch_y, quitting;
static int shot_every;
static int speed = 1;                    /* F8: 1, 2, 4 or 8 times as fast */

void arm7_save_flush(void);
void snd_mix(int16_t *out, int n);

/* ------------------------------------------------------------------ audio */

#define AUDIO_RATE 32768
#define NBUF 12
static HWAVEOUT wave;
static WAVEHDR hdrs[NBUF];
static int16_t bufs[NBUF][1200 * 2];
static int next_buf;
static double sample_frac;
static FILE *wav_dump;

static void audio_init(void) {
    const char *dump = getenv("B2W2_WAV");
    if (dump && (wav_dump = fopen(dump, "wb"))) {
        /* sizes left at their maximum: the file is cut off whenever the run ends */
        static const uint8_t h[44] = {'R','I','F','F',0xff,0xff,0xff,0x7f,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0,
                                      0x00,0x80,0,0,0x00,0x00,2,0,4,0,16,0,'d','a','t','a',0xff,0xff,0xff,0x7f};
        fwrite(h, 1, 44, wav_dump);
    }
    if (!wnd) return;
    WAVEFORMATEX f = {0};
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = 2;
    f.nSamplesPerSec = AUDIO_RATE;
    f.wBitsPerSample = 16;
    f.nBlockAlign = 4;
    f.nAvgBytesPerSec = AUDIO_RATE * 4;
    if (waveOutOpen(&wave, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) wave = NULL;
}

/* One video frame of sound: about 548 samples, queued to the sound card unless it is already far ahead (fast
 * forward), in which case the frame is dropped */
static void audio_frame(void) {
    sample_frac += AUDIO_RATE * 560190.0 / RC_CLOCK;
    int n = (int)sample_frac;
    sample_frac -= n;
    WAVEHDR *h = &hdrs[next_buf];
    int free_buf = !wave || !(h->dwFlags & WHDR_PREPARED) || (h->dwFlags & WHDR_DONE);
    int16_t *out = free_buf ? bufs[next_buf] : NULL;
    static int16_t scratch[1200 * 2];
    snd_mix(out ? out : scratch, n);
    if (wav_dump) fwrite(out ? out : scratch, 4, (size_t)n, wav_dump);
    if (!wave || !out) return;
    if (h->dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(wave, h, sizeof *h);
    memset(h, 0, sizeof *h);
    h->lpData = (LPSTR)out;
    h->dwBufferLength = (DWORD)n * 4;
    waveOutPrepareHeader(wave, h, sizeof *h);
    waveOutWrite(wave, h, sizeof *h);
    next_buf = (next_buf + 1) % NBUF;
}

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

/* What the window shows: the two screens (each GPU3D_W x 192, the DS picture in the middle 256), placed by layout:
 *   0 stacked: top above bottom          1 big top: top screen fills the window, touch screen inset bottom right
 *   2 side by side                       3 top screen only (no touch screen: the mouse does nothing)
 * Widescreen shows all of the top screen's width (16:9), otherwise its middle 256 pixels (4:3). The touch screen is
 * always 4:3: its 2D picture doesn't extend past 256. Everything keeps its aspect ratio and is centred. */
static uint32_t scr_top[GPU3D_W * 192], scr_bot[GPU3D_W * 192];
static int widescreen = 1, layout = 0, fullscreen;
static RECT top_rect, bot_rect;                  /* where the screens were drawn, in client pixels */
static WINDOWPLACEMENT saved_place = {sizeof saved_place};
static LONG saved_style;

static void place_screens(int cw, int ch) {
    int tw = widescreen ? GPU3D_W : 256;
    double W, H;
    if (layout == 2) { W = tw + 256; H = 192; }
    else if (layout == 1 || layout == 3) { W = tw; H = 192; }
    else { W = tw; H = 384; }
    double s = cw / W < ch / H ? cw / W : ch / H;
    int ox = (int)((cw - W * s) / 2), oy = (int)((ch - H * s) / 2);
    SetRect(&top_rect, ox, oy, ox + (int)(tw * s), oy + (int)(192 * s));
    if (layout == 3) {
        SetRect(&bot_rect, 0, 0, 0, 0);
    } else if (layout == 2) {
        SetRect(&bot_rect, top_rect.right, oy, top_rect.right + (int)(256 * s), oy + (int)(192 * s));
    } else if (layout == 1) {
        double bs = s / 3 < 1 ? 1 : s / 3;       /* the inset: a third of the size, at least 1:1 */
        int m = (int)(8 * s / 3);
        SetRect(&bot_rect, top_rect.right - (int)(256 * bs) - m, top_rect.bottom - (int)(192 * bs) - m,
                top_rect.right - m, top_rect.bottom - m);
    } else {
        int bx = ox + (int)((tw - 256) * s / 2);
        SetRect(&bot_rect, bx, top_rect.bottom, bx + (int)(256 * s), top_rect.bottom + (int)(192 * s));
    }
}

static void present(void) {
    RECT cr;
    GetClientRect(wnd, &cr);
    int cw = cr.right, ch = cr.bottom;
    if (cw <= 0 || ch <= 0) return;
    place_screens(cw, ch);
    HDC dc = GetDC(wnd);
    /* Compose off screen, then copy once: no flicker between the black margins and the pictures */
    static HDC mem;
    static HBITMAP bmp;
    static int bw, bh;
    if (!mem) mem = CreateCompatibleDC(dc);
    if (cw != bw || ch != bh) {
        if (bmp) DeleteObject(bmp);
        bmp = CreateCompatibleBitmap(dc, cw, ch);
        SelectObject(mem, bmp);
        bw = cw; bh = ch;
    }
    FillRect(mem, &cr, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SetStretchBltMode(mem, COLORONCOLOR);
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = GPU3D_W;
    bi.bmiHeader.biHeight = -192;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    int tx = widescreen ? 0 : GPU3D_OFS, tw = widescreen ? GPU3D_W : 256;
    /* StretchDIBits counts source rows from the bottom of a top-down DIB's last row: pass 192 rows from row 0 */
    StretchDIBits(mem, top_rect.left, top_rect.top, top_rect.right - top_rect.left, top_rect.bottom - top_rect.top,
                  tx, 0, tw, 192, scr_top, &bi, DIB_RGB_COLORS, SRCCOPY);
    if (bot_rect.right > bot_rect.left)
        StretchDIBits(mem, bot_rect.left, bot_rect.top, bot_rect.right - bot_rect.left, bot_rect.bottom - bot_rect.top,
                      GPU3D_OFS, 0, 256, 192, scr_bot, &bi, DIB_RGB_COLORS, SRCCOPY);
    BitBlt(dc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
    ReleaseDC(wnd, dc);
}

static void set_fullscreen(int on) {
    if (on == fullscreen) return;
    fullscreen = on;
    if (on) {
        GetWindowPlacement(wnd, &saved_place);
        saved_style = GetWindowLongA(wnd, GWL_STYLE);
        MONITORINFO mi = {sizeof mi};
        GetMonitorInfoA(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongA(wnd, GWL_STYLE, (saved_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowLongA(wnd, GWL_STYLE, saved_style);
        SetWindowPlacement(wnd, &saved_place);
        SetWindowPos(wnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
}

static void set_title(void) {
    char t[128];
    sprintf(t, "Pocket Monsters White 2  (F11 fullscreen, F10 layout, F9 widescreen, F8 speed)%s",
            speed == 2 ? "  [2x]" : speed == 4 ? "  [4x]" : speed == 8 ? "  [8x]" : "");
    SetWindowTextA(wnd, t);
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE: case WM_DESTROY:
        quitting = 1;
        return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (wp == VK_F11 || (wp == VK_RETURN && (lp & (1 << 29)))) { set_fullscreen(!fullscreen); return 0; }
        if (wp == VK_F10) { layout = (layout + 1) % 4; return 0; }
        if (wp == VK_F9) { widescreen = !widescreen; return 0; }
        if (wp == VK_F8) { speed = speed == 8 ? 1 : speed * 2; set_title(); return 0; }
        break;
    case WM_SYSCHAR:
        if (wp == VK_RETURN) return 0;           /* no beep for Alt+Enter */
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN: case WM_MOUSEMOVE: case WM_LBUTTONUP: {
        /* Wherever the touch screen is drawn, map the cursor into its 256 x 192 */
        int mx = (short)LOWORD(lp), my = (short)HIWORD(lp);
        int w = bot_rect.right - bot_rect.left, hh = bot_rect.bottom - bot_rect.top;
        int inside = w > 0 && hh > 0 && mx >= bot_rect.left && mx < bot_rect.right && my >= bot_rect.top &&
                     my < bot_rect.bottom;
        int down_now = (wp & MK_LBUTTON) && msg != WM_LBUTTONUP;
        if (msg == WM_LBUTTONDOWN) { touch_down = inside; SetCapture(h); }
        else if (!down_now) touch_down = 0;
        if (touch_down && w > 0 && hh > 0) {
            int x = (mx - bot_rect.left) * 256 / w, y = (my - bot_rect.top) * 192 / hh;
            touch_x = x < 0 ? 0 : x > 255 ? 255 : x;
            touch_y = y < 0 ? 0 : y > 191 ? 191 : y;
        }
        if (msg == WM_LBUTTONUP) ReleaseCapture();
        return 0;
    }
    }
    return DefWindowProcA(h, msg, wp, lp);
}

void fe_init(void) {
    const char *e = getenv("B2W2_SHOT_EVERY");
    shot_every = e ? atoi(e) : 0;
    if (getenv("B2W2_HEADLESS")) { audio_init(); return; }
    SetProcessDPIAware();
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "b2w2";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    RECT r = {0, 0, GPU3D_W * 2, 384 * 2};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    wnd = CreateWindowA("b2w2", "Pocket Monsters White 2  (F11 fullscreen, F10 layout, F9 widescreen, F8 speed)",
                        WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                        r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&next_frame);
    timeBeginPeriod(1);
    audio_init();
    if (getenv("B2W2_FULLSCREEN")) set_fullscreen(1);
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
    int alt = focus && down(VK_MENU);                /* Alt+Enter toggles fullscreen; it isn't Start */
    for (int i = 0; i < 10; i++) if (focus && down(map[i]) && !(alt && map[i] == VK_RETURN)) k &= ~(1u << i);
    fe_keys = k;
    /* X, Y and Debug as the ARM7 leaves them at 0x02ffffa8 for the SDK's pad reading (func_0203d980): bits 10, 11
     * and 13, 0 = pressed, ORed into KEYINPUT; bit 15 means the lid is closed and blanks all keys */
    uint16_t ext = script_ext;
    if (focus && down('S')) ext &= ~0x0400u;
    if (focus && down('A')) ext &= ~0x0800u;
    fe_extkeys = ext;
    wr16(0x02ffffa8, ext);
    arm7_touch(touch_down, touch_x, touch_y);
    if (focus && down(VK_ESCAPE)) {
        if (fullscreen) set_fullscreen(0);       /* Esc leaves fullscreen first */
        else quitting = 1;
    }
}

int fe_render_wanted(void) {
    static int always = -1;
    if (always < 0) always = getenv("B2W2_RENDER") != NULL;    /* headless, but draw every frame like a window */
    return wnd || always || (shot_every > 0 && (rc_frames + 1) % (uint64_t)shot_every == 0);
}

void fe_vblank(void) {
    int shot = shot_every > 0 && rc_frames % (uint64_t)shot_every == 0;
    if (wnd || shot) {
        /* POWCNT1 bit 15: engine A on the top screen, otherwise on the lower (touch) screen */
        if (io16(0x304) & 0x8000) render_frame(scr_top, scr_bot);
        else render_frame(scr_bot, scr_top);
    }
    if (shot) {
        static uint32_t both[GPU3D_W * 384];
        memcpy(both, scr_top, sizeof scr_top);
        memcpy(both + GPU3D_W * 192, scr_bot, sizeof scr_bot);
        char name[64];
        sprintf(name, "shot_%06llu.png", (unsigned long long)rc_frames);
        write_png(name, both, GPU3D_W, 384);
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
    audio_frame();
    if (wav_dump && rc_frames % 60 == 0) fflush(wav_dump);
    if (!wnd) return;

    if (speed == 1 || rc_frames % (uint64_t)speed == 0) present();     /* sped up: show every speed-th frame */

    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
    if (quitting) {
        arm7_save_flush();
        ExitProcess(0);
    }

    /* 59.83 frames a second times the F8 speed, or as fast as it goes while Tab is held */
    LONGLONG period = freq.QuadPart * 560190 / RC_CLOCK / speed;
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
