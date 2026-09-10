/* ejay2.c - drive Dance eJay 2's shipped engine and graphics DLLs directly.
 *
 * PXD32D4.DLL (audio) and PXD32CL1.DLL (graphics) are native 32-bit code whose
 * imports are all DLLs Windows still ships, so they load and run as they are.
 * Nothing here is recompiled: this is the hybrid boundary from the other
 * direction, and its job is to establish the call protocol against real code
 * before Dancejay.exe gets lifted into that seat.
 *
 * The sequence is not the 1997 one and not a guess. Dancejay.exe is VB5 native,
 * which does not put `Declare Function` targets in the import table - it emits a
 * lazy thunk per API holding the DLL and export name. tools/vb_declares.py
 * recovers that mapping and then disassembles every call site, so the order and
 * the arguments below are read out of the shipping program:
 *
 *     ATyp(3) -> ADevice(0) -> AInit(hwnd) -> GFX_AnimationSwapInit(400, 400)
 *
 * ATyp first is the part that matters. It selects the engine profile, and
 * ADevice enumerates against it - which is why calling AInit first (the 1997
 * order) returned zero from everything.
 *
 * Volume is pinned low on purpose - a bring-up run queues whatever the mixer
 * happens to hold.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <mmsystem.h>

#define ENGINE "PXD32D4.DLL"
#define GFXDLL "PXD32CL1.DLL"

/* Win16 PASCAL became __stdcall in the 32-bit port; the exports are
 * undecorated because they come from a .DEF file.
 *
 * The argument sizes are not guesses either: a __stdcall callee ends `ret N`,
 * so N is read straight out of the DLL. AInit ends `ret 4` and takes the window
 * handle - passing nothing leaks four bytes of stack per call, which is how the
 * first version of this host segfaulted. */
typedef int (__stdcall *fn_i_i)(int);
typedef int (__stdcall *fn_i_v)(void);
typedef int (__stdcall *fn_i_ii)(int, int);
typedef int (__stdcall *fn_i_p)(void *);
typedef int (__stdcall *fn_i_5)(int, int, int, void *, void *);
typedef int (__stdcall *fn_i_4)(int, int, int, void *);
typedef int (__stdcall *fn_i_pii)(const char *, int, int);

/* ALoad's argument, recovered from its own code: it OpenFile()s the name at
 * +0x14, parses the BMP (LZ-decompressing it first if the magic is SZDD, which
 * three of the GRAFIKA files are), CreateDIBSection()s it and hands back the DC
 * and the pixels. So the graphics DLL never sees a file - it is given a live
 * DIB, and eJay's whole art pipeline is this one call.
 *
 * The name field is a VB fixed-length string, stored inline, 220 chars. */
typedef struct {
    int   hdc;      /* memory DC with the DIB selected */
    int   w;        /* biWidth  */
    int   h;        /* biHeight */
    void *bits;     /* DIB section pixels */
    void *pal;      /* the BITMAPINFO colour table, 256 RGBQUADs */
    char  name[220];
} ALOADREC;

static HMODULE g_eng, g_gfx;
static fn_i_v  g_dplayupd;

static FARPROC need(HMODULE h, const char *name)
{
    FARPROC p = GetProcAddress(h, name);
    if (!p) printf("  ! %s not found\n", name);
    return p;
}

/* ---- the window the graphics DLL draws into ---------------------------- */
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    /* The graphics DLL owns this client area: it GetDC()s the window and blits
     * straight into it, so Windows must not erase behind it. */
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

static HWND make_window(int cx, int cy)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    /* No background brush: PXD32CL1 GetDC()s this window and BitBlts straight
     * into it, so letting Windows erase on every invalidation would wipe the
     * DLL's output between frames. */
    wc.hbrBackground = NULL;
    wc.lpszClassName = "ejay2_host";
    RegisterClassA(&wc);
    RECT r = { 0, 0, cx, cy };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND h = CreateWindowExA(0, "ejay2_host", "Dance eJay 2 - engine driven directly",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top,
                             NULL, NULL, wc.hInstance, NULL);
    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);
    /* One black fill, so a screenshot shows what the DLL drew rather than the
     * desktop showing through an unpainted window. */
    HDC dc = GetDC(h);
    PatBlt(dc, 0, 0, cx, cy, BLACKNESS);
    ReleaseDC(h, dc);
    return h;
}

static void pump_messages(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

/* Capture the client area and count what is not black. "The DLL returned 1" and
 * "the DLL drew something" are different claims, and only the second is the
 * goal. */
static int capture(HWND wnd, const char *path)
{
    RECT rc;
    GetClientRect(wnd, &rc);
    int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return 0;

    HDC wdc = GetDC(wnd), mdc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, w, h);
    HBITMAP old = (HBITMAP)SelectObject(mdc, bmp);
    BitBlt(mdc, 0, 0, w, h, wdc, 0, 0, SRCCOPY);

    int stride = ((w * 3) + 3) & ~3;
    long bytes = (long)stride * h;
    unsigned char *px = (unsigned char *)malloc((size_t)bytes);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;

    long lit = 0;
    if (px && GetDIBits(mdc, bmp, 0, h, px, &bi, DIB_RGB_COLORS)) {
        for (long i = 0; i < bytes; i++) if (px[i]) lit++;
        if (path) {
            BITMAPFILEHEADER fh;
            memset(&fh, 0, sizeof(fh));
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
            fh.bfSize = fh.bfOffBits + (DWORD)bytes;
            FILE *f = fopen(path, "wb");
            if (f) {
                fwrite(&fh, sizeof(fh), 1, f);
                fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
                fwrite(px, 1, (size_t)bytes, f);
                fclose(f);
                printf("  wrote %s\n", path);
            }
        }
    }
    free(px);
    SelectObject(mdc, old);
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(wnd, wdc);
    printf("  window: %ld of %ld bytes non-black\n", lit, bytes);
    return lit > 0;
}

/* Pin every wave-out device low. The engine sets its own level and a bring-up
 * run can queue anything, so this is a seatbelt rather than a preference. */
/* ---- did anything actually come out? ------------------------------------
 * PXD32D4 imports waveOutOpen and the mixer calls but no waveOutWrite: it
 * enumerates and sets volume through winmm and then plays through DirectSound
 * (CoCreateInstance, and an ADSoff export to turn it off again). So there is no
 * write call to tap, and waveOutSetVolume does not govern what you hear.
 *
 * The endpoint meter does, though. IAudioMeterInformation on the default render
 * device reports the peak the hardware is actually being fed, which settles
 * "the engine is streaming" versus "the engine is streaming silence" without
 * capturing anything.
 */
#include <mmdeviceapi.h>
#include <endpointvolume.h>

/* The SDK only declares these; the import library that defines them moves
 * about between SDK versions, so spell them out. */
static const GUID k_MMDeviceEnumerator =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID k_IMMDeviceEnumerator =
    { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID k_IAudioMeterInformation =
    { 0xC02216F6, 0x8C67, 0x4B5B, { 0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64 } };

static IAudioMeterInformation *g_meter;

static void meter_open(void)
{
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    if (FAILED(CoInitialize(NULL))) return;
    if (FAILED(CoCreateInstance(&k_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                &k_IMMDeviceEnumerator, (void **)&en)))
        return;
    if (SUCCEEDED(en->lpVtbl->GetDefaultAudioEndpoint(en, eRender, eConsole, &dev)))
        dev->lpVtbl->Activate(dev, &k_IAudioMeterInformation, CLSCTX_ALL,
                              NULL, (void **)&g_meter);
    if (dev) dev->lpVtbl->Release(dev);
    en->lpVtbl->Release(en);
}

static float meter_peak(void)
{
    float v = 0.0f;
    if (g_meter) g_meter->lpVtbl->GetPeakValue(g_meter, &v);
    return v;
}

static void quiet_the_devices(int percent)
{
    UINT n = waveOutGetNumDevs();
    DWORD one = (DWORD)((65535.0 * percent) / 100.0);
    DWORD both = (one & 0xFFFF) | (one << 16);
    for (UINT i = 0; i < n; i++) waveOutSetVolume((HWAVEOUT)(UINT_PTR)i, both);
    printf("  volume pinned to %d%% across %u device(s)\n", percent, n);
}


/* Load one bitmap through the engine's own loader and report what came back. */
static int load_bitmap(fn_i_p ALoad, ALOADREC *rec, const char *name)
{
    memset(rec, 0, sizeof(*rec));
    /* VB pads this field with spaces (LSet on a fixed string); a plain
     * NUL-terminated name works too and is one less thing to be wrong about,
     * since ALoad hands it straight to OpenFile. */
    snprintf(rec->name, sizeof(rec->name), "%s", name);
    ALoad(rec);
    printf("  ALoad(%-18s) -> hdc %08X  %dx%d  bits %p\n",
           name, rec->hdc, rec->w, rec->h, rec->bits);
    return rec->hdc != 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* a crash must not eat the trail */
    int ticks = 120, volume = 20, atyp = 3, dplay = 0, chan = 9;
    int intro = 1, scrcap = 0, frames = 0, verbose = 0;
    const char *gfxdir = "GRAFIKA";
    const char *shot = NULL;
    const char *playfile = "DINTRO.PXD";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ticks") && i + 1 < argc) ticks = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--volume") && i + 1 < argc) volume = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--atyp") && i + 1 < argc) atyp = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gfx") && i + 1 < argc) gfxdir = argv[++i];
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--chan") && i + 1 < argc) chan = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--play") && i + 1 < argc) { playfile = argv[++i]; dplay = 1; }
        else if (!strcmp(argv[i], "--dplay")) dplay = 1;
        else if (!strcmp(argv[i], "--no-intro")) intro = 0;
        else if (!strcmp(argv[i], "--scrcap")) scrcap = 1;
        else if (!strcmp(argv[i], "--verbose")) verbose = 1;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
    }

    printf("Dance eJay 2 - driving the 1999 DLLs directly\n\n");
    g_eng = LoadLibraryA(ENGINE);
    g_gfx = LoadLibraryA(GFXDLL);
    if (!g_eng) { printf("cannot load %s (%lu)\n", ENGINE, GetLastError()); return 1; }
    printf("  %s at %p\n", ENGINE, (void *)g_eng);
    if (g_gfx) printf("  %s at %p\n", GFXDLL, (void *)g_gfx);

    meter_open();

    HWND wnd = make_window(640, 480);
    printf("  window %p\n", wnd);

    fn_i_i ATyp     = (fn_i_i) need(g_eng, "ATyp");
    fn_i_i ADevice  = (fn_i_i) need(g_eng, "ADevice");
    fn_i_i AInit    = (fn_i_i) need(g_eng, "AInit");      /* ret 4: takes hwnd */
    fn_i_p ALoad    = (fn_i_p) need(g_eng, "ALoad");
    fn_i_i AFenster = (fn_i_i) GetProcAddress(g_eng, "AFenster");
    fn_i_v ATimer   = (fn_i_v) GetProcAddress(g_eng, "ATimer");
    fn_i_i ALautSet = (fn_i_i) GetProcAddress(g_eng, "ALautSet");

    /* ---- Dancejay.exe's own order, read off its call sites -------------- */
    printf("  ATyp(%d)                    -> %d\n", atyp, ATyp ? ATyp(atyp) : -1);
    /* ADevice probes formats and returns a tier, not a device count: 0 means
     * its first probe (44100Hz 16-bit stereo, WAVE_FORMAT_QUERY) was accepted.
     * AInit likewise returns the engine's error word, so 0 is success. Reading
     * those two as failures is what made the 1999 engine look dead. */
    printf("  ADevice(0)                 -> %d (0 = 44.1k/16/stereo accepted)\n",
           ADevice ? ADevice(0) : -1);
    printf("  AInit(hwnd)                -> %d (0 = no error)\n",
           AInit ? AInit((int)(INT_PTR)wnd) : -1);

    fn_i_ii SwapInit  = g_gfx ? (fn_i_ii) GetProcAddress(g_gfx, "GFX_AnimationSwapInit") : NULL;
    fn_i_i  SetActive = g_gfx ? (fn_i_i) GetProcAddress(g_gfx, "GFX_SetActiveWindow") : NULL;
    if (SetActive) printf("  GFX_SetActiveWindow(hwnd)  -> %d\n", SetActive((int)(INT_PTR)wnd));
    if (SwapInit)  printf("  GFX_AnimationSwapInit      -> %08X\n", SwapInit(400, 400));

    /* ---- the intro screen ---------------------------------------------
     * SEITEN, eJay's own layout file, lists three bitmaps under `:Intro` -
     * EJAY31, EJAY33, EJAY32 - and the graphics DLL wants them as live DIBs,
     * which is what ALoad returns. Each Init call takes (hdc, w, h, bits); the
     * screen also takes the 256-entry palette. */
    fn_i_v Capture    = g_gfx ? (fn_i_v) GetProcAddress(g_gfx, "GFX_IntroDoScrCapture") : NULL;
    fn_i_5 InitScreen = g_gfx ? (fn_i_5) GetProcAddress(g_gfx, "GFX_IntroInitScreen") : NULL;
    fn_i_4 InitCopy   = g_gfx ? (fn_i_4) GetProcAddress(g_gfx, "GFX_IntroInitScreenCopy") : NULL;
    fn_i_4 InitLeds   = g_gfx ? (fn_i_4) GetProcAddress(g_gfx, "GFX_IntroInitLeds") : NULL;
    fn_i_4 InitText   = g_gfx ? (fn_i_4) GetProcAddress(g_gfx, "GFX_IntroInitText") : NULL;
    fn_i_i Refresh    = g_gfx ? (fn_i_i) GetProcAddress(g_gfx, "GFX_IntroRefresh") : NULL;

    if (intro && ALoad && InitScreen) {
        ALOADREC scr, leds, text;
        char path[MAX_PATH];

        /* Off by default: it grabs the whole desktop and does not come back
         * promptly on a modern display. */
        if (scrcap && Capture) printf("  GFX_IntroDoScrCapture      -> %d\n", Capture());

        snprintf(path, sizeof(path), "%s\\EJAY31A", gfxdir);
        if (load_bitmap(ALoad, &scr, path)) {
            printf("  GFX_IntroInitScreen        -> %d\n",
                   InitScreen(scr.hdc, scr.w, scr.h, scr.bits, scr.pal));
            /* Dancejay hands the same DC back as the copy source with a null
             * pixel pointer when the display is deeper than 8bpp. */
            if (InitCopy) printf("  GFX_IntroInitScreenCopy    -> %d\n",
                                 InitCopy(scr.hdc, scr.w, scr.h, NULL));
        } else {
            printf("  ! no bitmap - run this from the ejay folder\n");
        }

        snprintf(path, sizeof(path), "%s\\EJAY33A", gfxdir);
        if (InitLeds && load_bitmap(ALoad, &leds, path))
            printf("  GFX_IntroInitLeds          -> %d\n",
                   InitLeds(leds.hdc, leds.w, leds.h, leds.bits));

        snprintf(path, sizeof(path), "%s\\EJAY32A", gfxdir);
        if (InitText && load_bitmap(ALoad, &text, path))
            printf("  GFX_IntroInitText          -> %d\n",
                   InitText(text.hdc, text.w, text.h, text.bits));
    }

    quiet_the_devices(volume);
    /* ALautSet is 0..32768 on an exponential curve, not a percentage - the
     * argument is divided by 32768 and fed through exp() to a DirectSound
     * attenuation. ALautSet(10) is silence, not a tenth, so a "quiet" bring-up
     * run that passes a percentage straight through proves nothing: nothing was
     * ever audible. */
    if (ALautSet) printf("  ALautSet(%d%% = %d/32768) -> %d\n", volume,
                         volume * 32768 / 100, ALautSet(volume * 32768 / 100));

    /* The 1999 engine has a family the 1997 one did not: DStart, DPlayFile,
     * DPlayUpdate, DPlayFileCheck. Direct playback of a file on one of twelve
     * channels - how eJay 2 previews a sample, and a far shorter path to sound
     * than driving the sequencer was.
     *
     *     AFenster(hwnd) -> DStart(0) -> DPlayFile(path, 0, channel)
     *
     * with channel = index + 9, all of it read off Dancejay.exe's call sites. */
    fn_i_i DCheck = (fn_i_i) GetProcAddress(g_eng, "DPlayFileCheck");
    if (dplay) {
        fn_i_i   DStart    = (fn_i_i) GetProcAddress(g_eng, "DStart");
        fn_i_pii DPlayFile = (fn_i_pii) GetProcAddress(g_eng, "DPlayFile");
        g_dplayupd         = (fn_i_v) GetProcAddress(g_eng, "DPlayUpdate");
        if (AFenster)  printf("  AFenster(hwnd)             -> %d\n", AFenster((int)(INT_PTR)wnd));
        if (DStart)    printf("  DStart(0)                  -> %d\n", DStart(0));
        if (DPlayFile) printf("  DPlayFile(%s, 0, %d) -> %d\n", playfile, chan,
                              DPlayFile(playfile, 0, chan));
        if (DCheck)    printf("  DPlayFileCheck(%d)          -> %d\n", chan, DCheck(chan));
    }

    float peak = 0.0f;
    DWORD t0 = GetTickCount();
    printf("\n  running %d ticks ...\n", ticks);
    for (int i = 0; i < ticks; i++) {
        if (ATimer) ATimer();
        if (g_dplayupd) g_dplayupd();
        /* Refresh's argument is a millisecond timestamp into the intro, not a
         * page number: the DLL compares it against 0xd48, 0xfb9, 0x1770, 0x1ac2,
         * 0x1c75, 0x50dc, 0x5d8e and 0x7148, so the sequence runs for 29
         * seconds. Feeding it real elapsed time is what Dancejay's form timer
         * does; a synthetic i*16 runs the animation at whatever rate the host
         * manages instead. */
        int rc = 0;
        if (intro && Refresh) rc = Refresh((int)(GetTickCount() - t0));
        pump_messages();
        float p = meter_peak();
        if (p > peak) peak = p;
        if (verbose && i % 25 == 0)
            printf("    tick %4d  %6lums  refresh %d\n", i,
                   (unsigned long)(GetTickCount() - t0), rc);
        if (frames && shot && i % frames == 0) {
            char f[MAX_PATH];
            snprintf(f, sizeof(f), "%s.%03d.bmp", shot, i / frames);
            capture(wnd, f);
        }
        Sleep(16);
    }
    printf("  endpoint peak during run   -> %.4f\n", peak);
    if (dplay && DCheck) printf("  DPlayFileCheck(%d) at end   -> %d\n", chan, DCheck(chan));
    if (dplay) {
        /* Zeit = time. A number that advanced with the wall clock is the
         * difference between "the call was accepted" and "the engine is
         * actually streaming the file". */
        fn_i_i DGetZeit = (fn_i_i) GetProcAddress(g_eng, "DGetZeit");
        if (DGetZeit) printf("  DGetZeit(%d) at end         -> %d\n", chan, DGetZeit(chan));
    }

    capture(wnd, shot);
    printf("  done\n");
    FreeLibrary(g_eng);
    if (g_gfx) FreeLibrary(g_gfx);
    return 0;
}
