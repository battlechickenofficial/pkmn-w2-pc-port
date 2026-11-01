/* Windows frontend: a resizable window or fullscreen with both screens (widescreen top screen), keyboard and mouse
 * input, sound, 60 Hz pacing.
 *
 * Keys: arrows = D-pad, X = A, Z = B, S = X, A = Y, Q = L, W = R, Enter = Start, Backspace = Select,
 *       mouse on the touch screen = stylus, Tab (held) = fast forward, F8 = speed 1x/2x/4x/8x, F11 or Alt+Enter = fullscreen,
 *       F10 = layout (top screen only, the default; stacked, big top screen, side by side), F9 = widescreen on/off, Esc = leave fullscreen / quit,
 *       F7 (or the title menu's extra item, titlemenu.c) = play another save file.
 * B2W2_HEADLESS=1 runs without a window; B2W2_SHOT_EVERY=n saves shot_<frame>.png every n frames. */
#include "internal.h"
#include "saves.h"
#include <mmsystem.h>
#include <stdlib.h>
#include <stdio.h>

uint16_t fe_keys = 0x03ff, fe_extkeys = 0x2c00;
static HWND wnd;
static LARGE_INTEGER freq, next_frame;
static int touch_down, touch_x, touch_y, quitting;
static int shot_every;
static uint64_t shot_from;                /* B2W2_SHOT_FROM: no screenshots before this frame */
static int speed = 1;                    /* F8: 1, 2, 4 or 8 times as fast */
static int to_menu;                      /* restarted on another save file: press Start, unpaced, until the title menu */
static int want_load;                    /* the title menu's save-file item or F7 (1), or its Continue (2): the
                                          * picker next frame */
static int press_a;                      /* frames left of an A the frontend presses for the player */
static int auto_continue;                /* restarted to continue a save file: Continue, once at the title menu */
static int loading;                      /* a save file chosen to play: black until the game has left the title menu
                                          * (its Continue flashing and fading), so the list goes straight to the game */
static int swallow_keys;                 /* after the picker: the game doesn't see keys until they are let go */

void arm7_save_flush(void);
int title_menu_keys(uint16_t *keys);
void top_screen_prepare(int on);
void top_screen_compose(uint32_t *top);
void top_screen_keys(uint16_t *keys);
void snd_mix(int16_t *out, int n);
void snd_menu_sound(int k);

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
    int16_t *out = free_buf && !to_menu ? bufs[next_buf] : NULL;
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
static int widescreen = 1, layout = 3, fullscreen;
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
    /* Top screen only, while the game has its main engine on the touch screen (the party list from the field menu):
     * that's where everything is, and the top screen is only decoration, so show the touch screen in its place */
    const uint32_t *shown_top = layout == 3 && !(io16(0x304) & 0x8000) ? scr_bot : scr_top;
    if (!loading)
        StretchDIBits(mem, top_rect.left, top_rect.top, top_rect.right - top_rect.left, top_rect.bottom - top_rect.top,
                      tx, 0, tw, 192, shown_top, &bi, DIB_RGB_COLORS, SRCCOPY);
    if (!loading && bot_rect.right > bot_rect.left)
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
        if (picker_active()) {
            /* a key already held when the picker opened (the game's A that confirmed the save) only repeats: not a choice */
            int held = (lp & (1 << 30)) && (wp == VK_RETURN || wp == 'X' || wp == 'Z' || wp == VK_ESCAPE);
            if (held) return 0;
            if (wp == VK_UP) picker_key(PK_UP);
            else if (wp == VK_DOWN) picker_key(PK_DOWN);
            else if (wp == VK_LEFT || wp == 'Q') picker_key(PK_PAGE_UP);
            else if (wp == VK_RIGHT || wp == 'W') picker_key(PK_PAGE_DOWN);
            else if (wp == 'X' || wp == VK_RETURN) picker_key(PK_A);
            else if (wp == 'Z' || wp == VK_ESCAPE || wp == VK_BACK) picker_key(PK_B);
            return 0;
        }
        if (wp == VK_F7) { want_load = 1; return 0; }
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
        if (picker_active()) return 0;
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
    if ((e = getenv("B2W2_SHOT_FROM"))) shot_from = strtoull(e, NULL, 10);
    saves_init();
    if ((e = getenv("B2W2_LAYOUT"))) layout = atoi(e) & 3;      /* 0 stacked, 1 big top, 2 side by side, 3 top only */
    to_menu = getenv("B2W2_TO_MENU") != NULL;
    auto_continue = loading = getenv("B2W2_CONTINUE") != NULL;
    SetEnvironmentVariableA("B2W2_TO_MENU", NULL);
    SetEnvironmentVariableA("B2W2_CONTINUE", NULL);
    if (getenv("B2W2_HEADLESS")) { audio_init(); return; }
    SetProcessDPIAware();
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "b2w2";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    RECT r = {0, 0, GPU3D_W * 3, 192 * 3};          /* the top screen only, three times over */
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    wnd = CreateWindowA("b2w2", "", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                        r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    /* Restarted on another save file: the window as it was */
    WINDOWPLACEMENT wp = {sizeof wp};
    int fs = 0;
    const char *w = getenv("B2W2_WINDOW");
    if (w && sscanf(w, "%ld %ld %ld %ld %u %d %d %d %d", &wp.rcNormalPosition.left, &wp.rcNormalPosition.top,
                    &wp.rcNormalPosition.right, &wp.rcNormalPosition.bottom, &wp.showCmd, &fs, &layout, &widescreen,
                    &speed) == 9)
        SetWindowPlacement(wnd, &wp);
    else
        ShowWindow(wnd, SW_SHOW);
    set_title();
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&next_frame);
    timeBeginPeriod(1);
    audio_init();
    if (getenv("B2W2_FULLSCREEN") || fs) set_fullscreen(1);
}

/* Play another save file: the game reads its save chip as it boots, so start again on the new file, in the same
 * window state, and press through to the title menu */
static void switch_save(const char *name, int cont) {
    arm7_save_flush();
    saves_set_current(name);
    WINDOWPLACEMENT wp = {sizeof wp};
    if (fullscreen) wp = saved_place;
    else GetWindowPlacement(wnd, &wp);
    char env[160];
    sprintf(env, "%ld %ld %ld %ld %u %d %d %d %d", wp.rcNormalPosition.left, wp.rcNormalPosition.top,
            wp.rcNormalPosition.right, wp.rcNormalPosition.bottom, wp.showCmd == SW_SHOWMAXIMIZED ? SW_SHOWMAXIMIZED :
            SW_SHOWNORMAL, fullscreen, layout, widescreen, speed);
    SetEnvironmentVariableA("B2W2_WINDOW", env);
    SetEnvironmentVariableA("B2W2_TO_MENU", "1");
    if (cont) SetEnvironmentVariableA("B2W2_CONTINUE", "1");
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (CreateProcessW(NULL, GetCommandLineW(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        ExitProcess(0);
    }
    rc_logf("couldn't restart on save file %s (error %lu)\n", name, GetLastError());
}

/* The picker, drawn on the screens of the paused game, until the user chooses. 1 and the file's name, or -1.
 * Without a window it is drawn once to picker.png and cancelled (for testing). */
static int run_picker(int mode, char name[64]) {
    static uint32_t keep_top[GPU3D_W * 192], keep_bot[GPU3D_W * 192];
    memcpy(keep_top, scr_top, sizeof keep_top);
    memcpy(keep_bot, scr_bot, sizeof keep_bot);
    picker_open(mode);
    loading = 0;                             /* (a list over the black of a save file loading: shown) */
    int r;
    if (!wnd) {
        static uint32_t both[GPU3D_W * 384];
        picker_render(both, both + GPU3D_W * 192);
        write_png("picker.png", both, GPU3D_W, 384);
        picker_close();
        /* testing: the file to choose; with commas, one after another each time the list comes up (an empty one: B) */
        static int picks;
        const char *pick = getenv("B2W2_PICK");
        for (int k = 0; pick && k < picks && strchr(pick, ','); k++) pick = strchr(pick, ',') + 1;
        picks++;
        if (pick && strcspn(pick, ",")) {
            size_t n = strcspn(pick, ",");
            if (n > 63) n = 63;
            memcpy(name, pick, n);
            name[n] = 0;
            return 1;
        }
        return -1;
    }
    /* the key that opened the list (seen by polling) may still be waiting as a message: it isn't a choice */
    MSG km;
    while (PeekMessageA(&km, wnd, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE)) {}
    /* at the game's frame rate, mixing sound as it goes: the music carries on, and the menu sounds play */
    LONGLONG period = freq.QuadPart * 560190 / RC_CLOCK;
    LARGE_INTEGER next, now;
    QueryPerformanceCounter(&next);
    int linger = -1;                     /* after choosing: a moment for its sound (and before a restart) */
    for (;;) {
        MSG m;
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
        memcpy(scr_top, keep_top, sizeof keep_top);
        memcpy(scr_bot, keep_bot, sizeof keep_bot);
        if (picker_active()) picker_render(scr_top, scr_bot);
        present();
        audio_frame();
        r = picker_result(name);
        if (r && linger < 0) linger = mode == PICK_LOAD && r == 1 ? 20 : 10;
        if (quitting || (r && linger-- == 0)) break;
        next.QuadPart += period;
        QueryPerformanceCounter(&now);
        if (now.QuadPart > next.QuadPart + period * 4) next = now;
        while (now.QuadPart < next.QuadPart) {
            LONGLONG ms = (next.QuadPart - now.QuadPart) * 1000 / freq.QuadPart;
            if (ms > 1) Sleep((DWORD)(ms - 1));
            QueryPerformanceCounter(&now);
        }
    }
    picker_close();
    memcpy(scr_top, keep_top, sizeof keep_top);
    memcpy(scr_bot, keep_bot, sizeof keep_bot);
    /* (saving, that frame still has the field menu, and choosing a file to play, the title menu: the next ones haven't) */
    if (mode != PICK_SAVE && r != 1) present();
    swallow_keys = 1;
    QueryPerformanceCounter(&next_frame);
    return quitting ? -1 : r;
}

/* The picture kept with a save: the field's 3D view as the game saved, without its text boxes (they're 2D), or the
 * top screen as shown when the 3D engine isn't on it */
static void save_picture(const char *name) {
    static uint32_t px[GPU3D_W * 192];
    const uint32_t *fb = gpu3d_framebuffer();
    if ((io16(0x304) & 0x8000) && (io32(0) & 8) && fb) {
        for (int i = 0; i < GPU3D_W * 192; i++) {
            uint32_t c = fb[i], r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
            px[i] = (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
        }
    } else {
        memcpy(px, scr_top, sizeof px);
    }
    char path[MAX_PATH];
    saves_picture_path(path, name);
    write_png(path, px, GPU3D_W, 192);
}

/* ------------------------------------------------------------------ the report */

/* The field menu's report (レポート): the game asks whether to write the report over a panel of the game so far
 * (date, place, party, badges, Pokedex, play time). Here the list of save files comes up instead, with that panel;
 * choosing a file answers yes for the player (A until the report is written and its screen closes), and the save goes
 * to that file without asking again. B answers no. The panel is kept with the file (<name>.report.png). */
enum { REPORT_NONE, REPORT_WAIT, REPORT_YES, REPORT_NO };
static int report;
static uint64_t report_from;
static int report_gone;                  /* frames the report's screen has been away (it is while the save is written) */
static int report_written;               /* the save was written */
static char report_file[64];             /* chosen, and the save not yet started */
#define PANEL_SX 18                      /* the panel on the touch screen, its contents' columns */
#define PANEL_SY 52
#define PANEL_SW 192
#define PANEL_SH 113
static uint32_t report_panel[PANEL_SW * PANEL_SH];
static int report_decided;               /* chosen on pressing レポート, before its screen: REPORT_YES or REPORT_NO */
static int report_open;                  /* レポート was pressed in the field menu: the list at the next frame */
static int report_press;                 /* frames left of the A that hands レポート to the game, once a file is chosen */

/* The field menu's buttons are 2 x 3; the selected one has a blue face. A on the bottom left one, レポート. */
extern int render_keep_b;                /* render.h (its OPAQUE would meet windows.h's) */
extern uint32_t render_ui_b[192 * 256];

/* The press is kept from the game (until A is let go) while the list is up: backing out of it leaves the menu as it
 * was, and choosing a file presses A on レポート for the player, so the game starts its report then. */
static void report_menu_keys(uint16_t *k) {
    static uint16_t last = 0x3ff;
    static int hold;
    uint16_t pressed = last & ~*k;
    last = *k;
    if (hold) {
        if (*k & 1) hold = 0;
        else { *k |= 1; return; }
    }
    if (report_press) { *k &= ~1u; report_press--; return; }
    if (!(pressed & 1) || !rc_scene(SCENE_FIELD_MENU) || !render_keep_b || report_decided || report != REPORT_NONE) return;
    int blue = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t c = render_ui_b[(130 + i * 6) * 256 + 100 + i * 4];
        blue += (c & 255) > (c >> 16 & 255) + 40;
    }
    if (blue >= 3) { report_open = 1; hold = 1; *k |= 1; }
}

/* The list as soon as レポート is pressed; the game's report starts once a file is chosen (B: nothing happens) */
static void report_early(void) {
    if (!report_open) return;
    report_open = 0;
    snd_menu_sound(1);                       /* the game's own decide sound would come with its screen, held back */
    picker_set_current_panel(NULL, 0, 0);
    char name[64];
    /* the file chosen is the one in use from now: a save that follows another closely (the same writing, to the
     * game) goes there too */
    if (run_picker(PICK_SAVE, name) == 1) {
        strcpy(report_file, name);
        saves_set_current(name);
        report_decided = REPORT_YES;
        report_press = 2;
    }
}

static void save_panel(const char *name) {
    char path[MAX_PATH];
    saves_panel_path(path, name);
    if (report == REPORT_YES) write_png(path, report_panel, PANEL_SW, PANEL_SH);
    else DeleteFileA(path);                  /* saved some other way: the old panel would be out of date */
}

static void report_frame(void) {
    if (!rc_scene(SCENE_REPORT)) {
        if (report == REPORT_NONE) return;
        /* away while the save is written (and for a frame or two after): done once away a second after the save */
        if (report != REPORT_WAIT && ++report_gone < (report_written ? 60 : 300)) return;
        if (report == REPORT_YES && report_file[0]) {       /* written without a save starting (an empty chip's) */
            save_picture(report_file);
            save_panel(report_file);
        }
        rc_logf("report done at frame %llu\n", (unsigned long long)rc_frames);
        report = REPORT_NONE;
        report_file[0] = 0;
        report_written = report_gone = 0;
        report_decided = 0;
        return;
    }
    report_gone = 0;
    if (report == REPORT_NONE && report_decided) { report = REPORT_WAIT; report_from = rc_frames; }
    if (report == REPORT_NONE) { report = REPORT_WAIT; report_from = rc_frames; rc_logf("report up at frame %llu\n", (unsigned long long)rc_frames); }
    if (report != REPORT_WAIT || rc_frames < report_from + (report_decided ? 12 : 24)) return;   /* the panel is
                                                                                                  * drawn by then */
    /* the touch screen is engine B here, the field on engine A above it */
    const uint32_t *bottom = io16(0x304) & 0x8000 ? scr_bot : scr_top;
    for (int y = 0; y < PANEL_SH; y++)
        memcpy(report_panel + y * PANEL_SW, bottom + (PANEL_SY + y) * GPU3D_W + GPU3D_OFS + PANEL_SX, PANEL_SW * 4);
    picker_set_current_panel(report_panel, PANEL_SW, PANEL_SH);
    if (report_decided) { report = report_decided; report_decided = 0; return; }
    char name[64];
    if (run_picker(PICK_SAVE, name) == 1) {
        strcpy(report_file, name);
        saves_set_current(name);
        report = REPORT_YES;
    } else {
        report = REPORT_NO;
    }
}

/* The presses that answer the report's question, every 4 frames, the player's own keys held back meanwhile */
static void report_keys(uint16_t *k) {
    if (report == REPORT_NONE || !rc_scene(SCENE_REPORT)) return;
    *k = 0x03ff;
    if (report == REPORT_YES && rc_frames % 4 < 2) *k &= ~0x0001u;         /* A */
    if (report == REPORT_NO && rc_frames % 4 < 2) *k &= ~0x0002u;          /* B */
}

/* The report's event (func_ov012_021634f0, through 0215bf00; runtime.c calls this with the address of its state).
 * It sets up its window and the panel (states 0-1), asks whether to write (2: the question, 3: はい/いいえ, 4: the
 * answer), then writes: the panel and the field go into writing (5-8), the save (9), the jingle starts and the field
 * leaves writing (10), a last line and waits for it, the jingle and a key (11-16), the end (19-20). A file chosen on
 * the list goes straight from 2 to 5, so nothing is asked and the writing starts at once, and from 11 to the end, so
 * the player is free as soon as it is written (the jingle plays on); the list closed with B goes to the end the answer
 * いいえ takes (17). */
static uint64_t report_event_frame;      /* the last frame the report's event ran (and wasn't at its end) */

void fe_report_event(uint32_t state) {
    static int skipping;                     /* this report's question was skipped */
    uint32_t s = rd32(state);
    report_event_frame = s >= 19 ? 0 : rc_frames;
    if (s == 2) {
        skipping = 0;
        if (report_decided == REPORT_YES || report == REPORT_YES) { wr32(state, 5); skipping = 1; }
        else if (report_decided == REPORT_NO || report == REPORT_NO) wr32(state, 17);
    } else if (s == 11 && skipping) {
        wr32(state, 19);
        skipping = 0;
    }
}

/* A report is under way, from pressing レポート (and choosing a file) until its event has ended: the field menu
 * closing behind it and the band pointing down aren't shown (topscreen.c), and the game's decide sounds for the
 * presses made for the player aren't heard (snd.c) */
int fe_reporting(void) {
    return report_open || report_press || report_decided == REPORT_YES ||
           (report_event_frame && rc_frames <= report_event_frame + 2);
}

/* A save file chosen on the list is loading through the title menu (snd.c: its Continue makes no sound) */
int fe_loading(void) { return loading; }

/* The game started writing a save: ask which file it goes to (B: the one in use), unless the report's list just did */
static void pick_save_file(void) {
    char name[64];
    if (report_file[0]) {
        strcpy(name, report_file);
        report_file[0] = 0;
        report_written = 1;
        report_gone = 0;                     /* counting from the save */
    } else if (run_picker(PICK_SAVE, name) != 1) {
        strcpy(name, saves_current_name());
    }
    saves_set_current(name);
    rc_logf("save at frame %llu goes to %s\n", (unsigned long long)rc_frames, name);
    save_picture(name);
    save_panel(name);
    arm7_save_pick_done();
    arm7_save_flush();
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
    if (swallow_keys) {
        int any = down('S') || down('A') || down(VK_ESCAPE);
        for (int i = 0; i < 10; i++) any |= down(map[i]);
        if (!any) swallow_keys = 0;
    }
    if (swallow_keys) focus = 0;
    for (int i = 0; i < 10; i++) if (focus && down(map[i]) && !(alt && map[i] == VK_RETURN)) k &= ~(1u << i);
    /* Back to the title menu after a restart: Start, briefly, every half second */
    if (to_menu && rc_title_menu_frame) { to_menu = 0; rc_logf("title menu at frame %llu\n", (unsigned long long)rc_frames); }
    if (to_menu && rc_frames % 30 < 2) k &= ~0x0008u;
    int tm = title_menu_keys(&k);
    if (tm) want_load = tm;
    /* restarted to continue: Continue (first on the menu, and selected) a moment after the menu is up */
    static uint64_t menu_since;                                /* the frame the title menu came up */
    if (!rc_scene(SCENE_TITLE_MENU)) menu_since = 0;
    else if (!menu_since) menu_since = rc_frames;
    if (auto_continue && menu_since && rc_frames > menu_since + 30) { auto_continue = 0; press_a = 2; }
    if (press_a) { k &= ~0x0001u; press_a--; }                 /* past title_menu_keys: the game sees it */
    /* the title menu has been and gone (Continue pressed, the menu faded): the game's own black, then the field */
    if (loading && !auto_continue && !press_a && rc_title_menu_frame && !rc_scene(SCENE_TITLE_MENU)) loading = 0;
    report_menu_keys(&k);
    top_screen_keys(&k);
    report_keys(&k);
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
    int shot = shot_every > 0 && rc_frames % (uint64_t)shot_every == 0 && rc_frames >= shot_from;
    if (wnd || shot || getenv("B2W2_RENDER")) {
        /* POWCNT1 bit 15: engine A on the top screen, otherwise on the lower (touch) screen */
        if (io16(0x304) & 0x8000) render_frame(scr_top, scr_bot);
        else render_frame(scr_bot, scr_top);
        if (layout == 3) top_screen_compose(scr_top);
    }
    top_screen_prepare(layout == 3);
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
    report_frame();
    if (arm7_save_pick_pending()) pick_save_file();
    if (want_load) {
        /* the game's Continue: the file in use continues there and then; another restarts on it and continues */
        int cont = want_load == 2;
        want_load = 0;
        char name[64];
        if (run_picker(PICK_LOAD, name) == 1) {
            if (cont && !_stricmp(name, saves_current_name())) { press_a = 2; loading = 1; }
            else if (wnd) switch_save(name, cont);
        }
    }
    report_early();
    if (!wnd) return;

    /* sped up: show every speed-th frame; going back to the title menu, every 8th */
    if (to_menu ? rc_frames % 8 == 0 : speed == 1 || rc_frames % (uint64_t)speed == 0) present();

    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
    if (quitting) {
        if (arm7_save_pick_pending()) arm7_save_pick_done();     /* closed mid-save: the file in use */
        arm7_save_flush();
        ExitProcess(0);
    }

    /* 59.83 frames a second times the F8 speed, or as fast as it goes while Tab is held */
    LONGLONG period = freq.QuadPart * 560190 / RC_CLOCK / speed;
    next_frame.QuadPart += period;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (to_menu || (down(VK_TAB) && GetForegroundWindow() == wnd)) { next_frame = now; return; }
    if (now.QuadPart > next_frame.QuadPart + period * 4) next_frame = now;     /* fell behind: don't catch up */
    while (now.QuadPart < next_frame.QuadPart) {
        LONGLONG ms = (next_frame.QuadPart - now.QuadPart) * 1000 / freq.QuadPart;
        if (ms > 1) Sleep((DWORD)(ms - 1));
        QueryPerformanceCounter(&now);
    }
}
