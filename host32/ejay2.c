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
typedef int (__stdcall *fn_tex)(const char *, const char *, const char *, int, int, int);
typedef int (__stdcall *fn_sinit)(int, int, int, int, int, const char *, int);
typedef int (__stdcall *fn_zeich)(int, int, int, const char *, const char *, int, int);
typedef int (__stdcall *fn_aplay)(int, int, int, int, int, short *, const char *,
                                  int, int, int, int, int);

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
/* The workspace chrome and the sheet its controls are cut from, kept so the
 * window can be repainted from them. */
static ALOADREC g_screen, g_sheet;
/* Chrome + sample blocks, composited once. The cursor erases back to this
 * rather than to the bare chrome, or it would wipe the arrangement as it
 * swept across. */
static HDC g_canvas;
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
    if (m == WM_PAINT && g_screen.hdc) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        BitBlt(dc, 0, 0, g_screen.w, g_screen.h,
               g_canvas ? g_canvas : (HDC)(INT_PTR)g_screen.hdc, 0, 0, SRCCOPY);
        EndPaint(h, &ps);
        return 0;
    }
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

/* ---- what is the engine actually opening? --------------------------------
 * The placed sample is loaded on the audio thread, and a wrong path is not
 * reported anywhere - the status word simply never changes. Swapping one entry
 * in PXD32D4's import table for a logger answers it directly. */
static HFILE (WINAPI *g_real_openfile)(LPCSTR, LPOFSTRUCT, UINT);
static HANDLE (WINAPI *g_real_createfile)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                          DWORD, DWORD, HANDLE);

/* Samples do not come through OpenFile - that is ALoad's bitmap path. The
 * engine memory-maps them: CreateFileA, CreateFileMappingA, MapViewOfFile. */
static HANDLE WINAPI log_createfile(LPCSTR name, DWORD acc, DWORD share,
                                    LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                    DWORD flags, HANDLE tmpl)
{
    HANDLE h = g_real_createfile(name, acc, share, sa, disp, flags, tmpl);
    printf("  [CreateFile] %-52s -> %s\n", name ? name : "(null)",
           h == INVALID_HANDLE_VALUE ? "FAILED" : "ok");
    return h;
}

static HFILE WINAPI log_openfile(LPCSTR name, LPOFSTRUCT of, UINT style)
{
    HFILE h = g_real_openfile(name, of, style);
    printf("  [OpenFile] %-56s -> %d\n", name ? name : "(null)", (int)h);
    return h;
}

static int patch_import(HMODULE mod, const char *dll, const char *fn,
                        void *repl, void **orig)
{
    unsigned char *base = (unsigned char *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + rva); d->Name; d++) {
        if (_stricmp((char *)(base + d->Name), dll)) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + d->OriginalFirstThunk);
        IMAGE_THUNK_DATA *addrs = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, addrs++) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            IMAGE_IMPORT_BY_NAME *n = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((char *)n->Name, fn)) continue;
            DWORD old;
            if (!VirtualProtect(&addrs->u1.Function, sizeof(void *), PAGE_READWRITE, &old))
                return 0;
            *orig = (void *)(UINT_PTR)addrs->u1.Function;
            addrs->u1.Function = (UINT_PTR)repl;
            VirtualProtect(&addrs->u1.Function, sizeof(void *), old, &old);
            return 1;
        }
    }
    return 0;
}

/* Peek at the engine's own globals. Their addresses come out of the
 * disassembly, and watching them beats guessing which call quietly did
 * nothing: 0x4342C is the handshake AStart spins on, 0x43394 / 0x4339C /
 * 0x433A4 are the three gates AGetTime checks before it will report a
 * position, and the word at track0+0x6A counts the samples APlay has placed. */
static void peek(const char *when)
{
    const char *b = (const char *)g_eng;
    printf("  [%-12s] ready=%08lX astart=%d placed=%d | mixgate=%d armed=%d "
           "dslive=%d crit=%d mode=%d\n", when,
           (unsigned long)*(DWORD *)(b + 0x434A0),
           *(int *)(b + 0x4342C),
           *(short *)(b + 0x429C8 + 0x6A),
           *(short *)(b + 0x434A8),   /* AStart opens this; AStop shuts it  */
           *(short *)(b + 0x3AC68),   /* the mixer will not run without it  */
           *(short *)(b + 0x433EC),   /* DStart sets it: DirectSound live   */
           *(int   *)(b + 0x4336C),   /* critical sections initialised      */
           *(short *)(b + 0x39DA4));  /* 0 play, 1 export, 2 record         */
}

/* ---- the sample library --------------------------------------------------
 * A .PXD carries its own name in its header: "tPxD", then a NUL-terminated
 * string, and eJay writes it as two lines - "Snare Beat
Risk", "Perc.L
Vers10"
 * - which is exactly the pair of strings GFX_SampleZeichne wants for a block
 * label. So the library is self-describing and needs no index file, which is
 * just as well because there is not one.
 *
 * Dance eJay 2's own library ships on its second disc. eJay 1's is on ours, and
 * the two engines share the format - the 1999 engine opens, decodes and plays a
 * 1996 sample without being asked to do anything special about it.
 */
typedef struct {
    char path[MAX_PATH];
    char l1[24], l2[24];
} SAMPLE;

static SAMPLE g_lib[512];
static int    g_lib_n;

/* Names in the header are two lines separated by a newline, but not tidily:
 * some start with the separator (one-line names), and they carry stray control
 * characters that render as boxes. Trim, and promote a lone second line. */
static void tidy(char *t)
{
    size_t a = 0, b;
    while (t[a] && (unsigned char)t[a] < 32) a++;
    if (a) memmove(t, t + a, strlen(t + a) + 1);
    b = strlen(t);
    while (b && (unsigned char)t[b - 1] <= 32) t[--b] = 0;
}

static void split_name(const char *raw, char *l1, char *l2, size_t n)
{
    const char *br = strchr(raw, '\n');
    if (br) {
        size_t k = (size_t)(br - raw);
        if (k >= n) k = n - 1;
        memcpy(l1, raw, k); l1[k] = 0;
        snprintf(l2, n, "%s", br + 1);
    } else {
        snprintf(l1, n, "%s", raw);
        l2[0] = 0;
    }
    tidy(l1); tidy(l2);
    if (!l1[0] && l2[0]) { memcpy(l1, l2, n); l2[0] = 0; }
}

/* Walk the two-letter directories eJay files its samples in. */
static int scan_library(const char *root)
{
    char pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof(pat), "%s\\*", root);
    HANDLE dh = FindFirstFileA(pat, &fd);
    if (dh == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (strlen(fd.cFileName) != 2) continue;
        char sub[MAX_PATH];
        snprintf(sub, sizeof(sub), "%s\\%s\\*.PXD", root, fd.cFileName);
        WIN32_FIND_DATAA ff;
        HANDLE fh = FindFirstFileA(sub, &ff);
        if (fh == INVALID_HANDLE_VALUE) continue;
        do {
            if (g_lib_n >= (int)(sizeof(g_lib) / sizeof(g_lib[0]))) break;
            SAMPLE *sm = &g_lib[g_lib_n];
            snprintf(sm->path, sizeof(sm->path), "%s\\%s\\%s",
                     root, fd.cFileName, ff.cFileName);
            char hdr[64];
            FILE *f = fopen(sm->path, "rb");
            if (!f) continue;
            size_t got = fread(hdr, 1, sizeof(hdr) - 1, f);
            fclose(f);
            hdr[got] = 0;
            if (got < 8 || memcmp(hdr, "tPxD", 4)) continue;
            split_name(hdr + 4, sm->l1, sm->l2, sizeof(sm->l1));
            g_lib_n++;
        } while (FindNextFileA(fh, &ff) && g_lib_n < (int)(sizeof(g_lib)/sizeof(g_lib[0])));
        FindClose(fh);
    } while (FindNextFileA(dh, &fd));
    FindClose(dh);
    return g_lib_n;
}

/* ---- the names on the blocks --------------------------------------------
 * A .MIX is a saved arrangement, and the disc ships thirteen of them. Every
 * sample it places is stored as
 *
 *     01 <id:16> <len:16> <name> 00 ...        len = strlen + 2
 *
 * which is enough to read the names back without decoding the rest of the
 * format. Labelling the blocks with eJay's own sample names beats making some
 * up, and it is a first foothold in the song format for later.
 */
#define MAX_MIX_NAMES 64
static char g_names[MAX_MIX_NAMES][32];
static int  g_name_count;

static int load_mix_names(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = (unsigned char *)malloc((size_t)n);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); fclose(f); return 0; }
    fclose(f);

    for (long i = 0; i + 8 < n && g_name_count < MAX_MIX_NAMES; ) {
        if (d[i] != 1) { i++; continue; }
        int len = d[i + 3] | (d[i + 4] << 8);
        if (len < 4 || len > 32 || i + 5 + len > n) { i++; continue; }
        const unsigned char *nm = d + i + 5;
        int ok = nm[len - 2] == 0;
        for (int k = 0; ok && k < len - 2; k++)
            if (nm[k] < 32 || nm[k] > 126) ok = 0;
        if (!ok) { i++; continue; }
        memcpy(g_names[g_name_count], nm, (size_t)(len - 2));
        g_names[g_name_count][len - 2] = 0;
        g_name_count++;
        i += 5 + len - 1;
    }
    free(d);
    return g_name_count;
}

/* ---- the playback cursor ------------------------------------------------
 * The 1997 engine drew this itself, through its one BitBlt import. The 1999
 * engine does not - eJay 2 declares MoveToEx, LineTo and CreatePen from gdi32
 * and draws it in Visual Basic - so the host owns it here, which is the same
 * arrangement, just on the other side of the boundary.
 *
 * The geometry is measured off EJAY01A rather than guessed: the arrangement
 * field's dark ground runs x 48..596, the orange lane rules sit 18 pixels
 * apart from y 16 to y 323, and there are sixteen of them. Twice eJay 1's
 * eight.
 *
 * Position comes from DGetZeit, the engine's own byte offset into the stream,
 * so the line is synchronised to the audio by construction rather than by a
 * timer that happens to agree.
 */
#define GRID_X0  48
#define GRID_X1  596
#define GRID_Y0  16
#define GRID_Y1  323
#define BROWSE_X0 176
#define BROWSE_X1 539
#define BROWSE_Y0 366   /* below the transport bar that overlaps the panel */
#define BROWSE_Y1 472

/* One arrangement, used twice: APlay places these on the engine's tracks and
 * GFX_SampleZeichne draws the same list into the same lanes, so what is on the
 * screen is what is playing rather than a picture of what might be. */
typedef struct { int lane, bar, bars, lib; } SLOT;
static SLOT g_song[] = {
    { 0,  0, 4, 0 }, { 0,  8, 4, 0 }, { 1,  2, 4, 1 }, { 1, 10, 4, 1 },
    { 2,  0, 2, 2 }, { 2,  4, 2, 2 }, { 2,  8, 2, 2 }, { 2, 12, 2, 2 },
    { 3,  4, 4, 3 }, { 4,  0, 8, 4 }, { 5,  6, 4, 5 }, { 6,  8, 4, 6 },
    { 7, 12, 4, 7 },
};
static const int g_song_n = (int)(sizeof(g_song) / sizeof(g_song[0]));

static void draw_cursor(HWND wnd, double frac)
{
    static int last = -1;
    if (!g_screen.hdc) return;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    int x = GRID_X0 + (int)((GRID_X1 - GRID_X0) * frac);

    HDC dc = GetDC(wnd);
    /* Erase by copying the chrome back from the DIB the engine loaded, the
     * same way eJay 1's host repainted from its background DC. Without it the
     * cursor smears into a solid bar instead of moving. */
    if (last >= 0)
        BitBlt(dc, last, GRID_Y0, 2, GRID_Y1 - GRID_Y0,
               g_canvas ? g_canvas : (HDC)(INT_PTR)g_screen.hdc,
               last, GRID_Y0, SRCCOPY);
    PatBlt(dc, x, GRID_Y0, 2, GRID_Y1 - GRID_Y0, WHITENESS);
    ReleaseDC(wnd, dc);
    last = x;
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
#include <audiopolicy.h>

/* The SDK only declares these; the import library that defines them moves
 * about between SDK versions, so spell them out. */
static const GUID k_MMDeviceEnumerator =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID k_IMMDeviceEnumerator =
    { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID k_IAudioMeterInformation =
    { 0xC02216F6, 0x8C67, 0x4B5B, { 0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64 } };

static const GUID k_IAudioSessionManager =
    { 0xBFA971F1, 0x4D5E, 0x40BB, { 0x93, 0x5E, 0x96, 0x70, 0x39, 0xBF, 0xBE, 0xE4 } };

static IAudioMeterInformation *g_meter;
static ISimpleAudioVolume *g_sessionvol;

/* The engine only fills its DirectSound buffer while its mixer is armed. When
 * it is not, what DirectSound loops over is uninitialised - full-scale noise,
 * and no engine volume setting touches it. So hold the process session muted
 * for exactly as long as that flag is down. It is the one guarantee a bring-up
 * run cannot be loud. */
static void mute_unless_mixing(int armed)
{
    static int last = -1;
    if (!g_sessionvol || armed == last) return;
    g_sessionvol->lpVtbl->SetMute(g_sessionvol, armed ? FALSE : TRUE, NULL);
    printf("  mixer %s -> audio %s\n", armed ? "armed" : "disarmed",
           armed ? "on" : "muted");
    last = armed;
}

/* Clamp this process's own audio session. ALautSet is the engine's mixer level
 * and does nothing about a DirectSound buffer the engine has not filled - a
 * bring-up run can and does put full-scale noise on the endpoint at a 1% engine
 * volume. This is the knob that actually governs what leaves the process, so it
 * is the one to hold down while testing. */
static void clamp_session(int percent)
{
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    IAudioSessionManager *mgr = NULL;
    ISimpleAudioVolume *vol = NULL;
    if (FAILED(CoCreateInstance(&k_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                &k_IMMDeviceEnumerator, (void **)&en)))
        return;
    if (SUCCEEDED(en->lpVtbl->GetDefaultAudioEndpoint(en, eRender, eConsole, &dev)) &&
        SUCCEEDED(dev->lpVtbl->Activate(dev, &k_IAudioSessionManager, CLSCTX_ALL,
                                        NULL, (void **)&mgr)) &&
        SUCCEEDED(mgr->lpVtbl->GetSimpleAudioVolume(mgr, NULL, FALSE, &vol))) {
        vol->lpVtbl->SetMasterVolume(vol, percent / 100.0f, NULL);
        vol->lpVtbl->SetMute(vol, TRUE, NULL);   /* until the mixer says otherwise */
        printf("  process session volume     -> %d%%, muted\n", percent);
        g_sessionvol = vol;                      /* kept: the mute follows the mixer */
    }
    if (mgr) mgr->lpVtbl->Release(mgr);
    if (dev) dev->lpVtbl->Release(dev);
    en->lpVtbl->Release(en);
}

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
    int intro = 1, scrcap = 0, frames = 0, verbose = 0, main_screen = 0, samples = 0;
    int seq = 0, trace_files = 0, song = 0;
    const char *libdir = NULL;
    /* The SAMPLE block of FONTS reads: Small Fonts / normal / 10 / 1 / -1 / 6. */
    int fontsize = 10, face_a = 1, face_b = -1, face_c = 6;
    const char *gfxdir = "GRAFIKA";
    const char *shot = NULL;
    /* METRO.PXD, the metronome, is the only real sample on the install disc -
     * the library itself lives on the second one. DINTRO.PXD is a mix, not a
     * sample, and handing it to DPlayFile gets its bytes rendered as PCM. */
    const char *playfile = "METRO.PXD";
    const char *mixfile = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ticks") && i + 1 < argc) ticks = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--volume") && i + 1 < argc) volume = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--atyp") && i + 1 < argc) atyp = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gfx") && i + 1 < argc) gfxdir = argv[++i];
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--chan") && i + 1 < argc) chan = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--play") && i + 1 < argc) { playfile = argv[++i]; dplay = 1; }
        else if (!strcmp(argv[i], "--dplay")) dplay = 1;
        else if (!strcmp(argv[i], "--seq")) seq = 1;
        else if (!strcmp(argv[i], "--lib") && i + 1 < argc) libdir = argv[++i];
        else if (!strcmp(argv[i], "--song")) { song = 1; seq = 1; samples = 1;
                                                main_screen = 1; intro = 0; }
        else if (!strcmp(argv[i], "--trace-files")) trace_files = 1;
        else if (!strcmp(argv[i], "--no-intro")) intro = 0;
        else if (!strcmp(argv[i], "--main")) { main_screen = 1; intro = 0; }
        else if (!strcmp(argv[i], "--samples")) { samples = 1; main_screen = 1; intro = 0; }
        else if (!strcmp(argv[i], "--mix") && i + 1 < argc) mixfile = argv[++i];
        else if (!strcmp(argv[i], "--font") && i + 4 < argc) {
            fontsize = atoi(argv[++i]); face_a = atoi(argv[++i]);
            face_b = atoi(argv[++i]); face_c = atoi(argv[++i]);
        }
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

    if (libdir)
        printf("  sample library             -> %d samples under %s\n",
               scan_library(libdir), libdir);
    meter_open();
    clamp_session(volume);

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

    /* Every interesting export in PXD32D4 begins by comparing a global against
     * 0x2A90CB20 and returning 0 if it does not match - it is the engine's
     * "memory is up" flag, set at the end of the allocation pass AInit runs.
     * APlay silently does nothing without it, so read it rather than wonder. */
    if (trace_files)
        printf("  file hooks                 -> OpenFile %d, CreateFile %d\n",
               patch_import(g_eng, "KERNEL32.dll", "OpenFile",
                            (void *)log_openfile, (void **)&g_real_openfile),
               patch_import(g_eng, "KERNEL32.dll", "CreateFileA",
                            (void *)log_createfile, (void **)&g_real_createfile));
    peek("after AInit");

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

    /* ---- the workspace -------------------------------------------------
     * SEITEN calls this page `:Hauptbild` - main picture - and gives it two
     * bitmaps: EJAY01 is the 640x480 chrome, EJAY02 the 940x520 sheet every
     * button state is cut from. The application blits the first straight to
     * the window and then composites over it, which is what happens here. The
     * sixteen arrangement lanes, the transport, the faders and the sample
     * browser are all in that one image. */
    if (main_screen && ALoad) {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\EJAY01A", gfxdir);
        if (load_bitmap(ALoad, &g_screen, path)) {
            HDC dc = GetDC(wnd);
            BitBlt(dc, 0, 0, g_screen.w, g_screen.h,
                   (HDC)(INT_PTR)g_screen.hdc, 0, 0, SRCCOPY);
            ReleaseDC(wnd, dc);
        }
        snprintf(path, sizeof(path), "%s\\EJAY02A", gfxdir);
        load_bitmap(ALoad, &g_sheet, path);
    }

    /* ---- the arrangement -----------------------------------------------
     * The three texture pairs Dancejay registers are the block styles - the
     * marbled fills eJay draws a placed sample with - and each AddTexturePair
     * returns the 1-based index Zeichne selects with. Order matters and is the
     * opposite of the obvious one: GFX_SampleInit builds the grid and zeroes
     * its entry count, so registering textures first throws them away.
     *
     * Zeichne renders one block into the grid's own memory DC (the value
     * SampleInit returns), which is why the call carries no coordinates - the
     * caller blits it into whichever lane it belongs in. Its third argument is
     * the block width in pixels. */
    if (mixfile)
        printf("  %s -> %d sample names\\n", mixfile, load_mix_names(mixfile));

    if (samples && g_gfx && g_screen.hdc) {
        fn_tex   AddTex = (fn_tex)   GetProcAddress(g_gfx, "GFX_SampleAddTexturePair");
        fn_sinit SInit  = (fn_sinit) GetProcAddress(g_gfx, "GFX_SampleInit");
        fn_zeich Zeich  = (fn_zeich) GetProcAddress(g_gfx, "GFX_SampleZeichne");

        /* FONTS is eJay's typeface table, one block per screen resolution, and
         * its fourth section is `SAMPLE`: "Small Fonts", normal, 10. The height
         * is the LAST argument, not the third - GFX_SampleInit builds a
         * std::string from argument 6 in place and hands CreateFontA that
         * pointer as the face with argument 7 as nHeight. Passing 0 there gets
         * a font with no height and a block with no label on it. */
        int griddc = SInit ? SInit(18, 0xa00, face_a, face_b, face_c,
                                   "Small Fonts", fontsize) : 0;
        printf("  GFX_SampleInit              -> %08X (the grid memory DC)\n", griddc);
        int tex[3] = { 0, 0, 0 };
        if (AddTex) {
            tex[0] = AddTex("TEXTURE.BMP",  "TEXTURE2.BMP", "DANCE2.PAL",  12, 25, 112);
            tex[1] = AddTex("TEXTUREA.BMP", "TEXTURA2.BMP", "DANCE2A.PAL", 12, 25, 112);
            tex[2] = AddTex("TEXTUREB.BMP", "TEXTURB2.BMP", "DANCE2B.PAL", 12, 25, 112);
            printf("  GFX_SampleAddTexturePair    -> %d %d %d\n", tex[0], tex[1], tex[2]);
        }

        /* Composite onto a canvas so the playback cursor has the arrangement
         * to erase back to, not the empty chrome. */
        HDC wdc = GetDC(wnd);
        g_canvas = CreateCompatibleDC(wdc);
        SelectObject(g_canvas, CreateCompatibleBitmap(wdc, g_screen.w, g_screen.h));
        BitBlt(g_canvas, 0, 0, g_screen.w, g_screen.h,
               (HDC)(INT_PTR)g_screen.hdc, 0, 0, SRCCOPY);

        const int bar = (GRID_X1 - GRID_X0) / 16;   /* sixteen bars across */
        int drawn = 0;
        if (Zeich && griddc) {
            for (int i = 0; i < g_song_n; i++) {
                const SLOT *sl = &g_song[i];
                int w = sl->bars * bar, style = tex[i % 3];
                const SAMPLE *sm = (sl->lib >= 0 && sl->lib < g_lib_n)
                                 ? &g_lib[sl->lib] : NULL;
                if (!style) continue;
                Zeich(style, 0, w, sm ? sm->l1 : "Sample", sm ? sm->l2 : "", 0, 0);
                BitBlt(g_canvas, GRID_X0 + sl->bar * bar, GRID_Y0 + sl->lane * 18 + 1,
                       w - 2, 16, (HDC)(INT_PTR)griddc, 0, 0, SRCCOPY);
                drawn++;
            }
        }
        printf("  blocks drawn into the grid -> %d\n", drawn);

        /* ---- the browser ------------------------------------------------
         * The bottom-centre panel is `G_SAMPLE_WINDOW` in eJay's own layout
         * table, with `K_SAMPLE_VSCROLL` down its right edge and the twelve
         * `B_GRUPPE_*` category buttons either side of it. It lists the samples
         * in the selected category, and you drag one from here up into a lane.
         * Measured off EJAY01A: x 176..539, y 350..472. */
        if (g_lib_n) {
            HFONT fnt = CreateFontA(-10, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                    DEFAULT_CHARSET, 0, 0, DEFAULT_QUALITY, 0,
                                    "Small Fonts");
            HFONT old = (HFONT)SelectObject(g_canvas, fnt);
            SetBkMode(g_canvas, TRANSPARENT);
            int rows = 0;
            while (rows < g_lib_n && BROWSE_Y0 + 6 + rows * 12 < BROWSE_Y1 - 12) {
                int y = BROWSE_Y0 + 6 + rows * 12;
                const SAMPLE *sm = &g_lib[rows];
                const char *file = strrchr(sm->path, '\\');
                SetTextColor(g_canvas, RGB(255, 190, 80));
                TextOutA(g_canvas, BROWSE_X0 + 8, y, sm->l1, (int)strlen(sm->l1));
                SetTextColor(g_canvas, RGB(160, 180, 240));
                TextOutA(g_canvas, BROWSE_X0 + 140, y, sm->l2, (int)strlen(sm->l2));
                SetTextColor(g_canvas, RGB(110, 130, 190));
                if (file) TextOutA(g_canvas, BROWSE_X0 + 280, y, file + 1,
                                   (int)strlen(file + 1));
                rows++;
            }
            SelectObject(g_canvas, old);
            DeleteObject(fnt);
            printf("  browser rows               -> %d of %d samples\n", rows, g_lib_n);
        }
        BitBlt(wdc, 0, 0, g_screen.w, g_screen.h, g_canvas, 0, 0, SRCCOPY);
        ReleaseDC(wnd, wdc);
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
    fn_i_i DCheck   = (fn_i_i) GetProcAddress(g_eng, "DPlayFileCheck");
    fn_i_i DGetZeit = (fn_i_i) GetProcAddress(g_eng, "DGetZeit");
    /* The decoded stream is the file minus its tPxD header, and DGetZeit is a
     * byte offset into it - so the cursor reaches the right-hand edge exactly
     * as the sample ends, with nothing to calibrate. */
    long total = 0;
    {
        FILE *f = fopen(playfile, "rb");
        if (f) { fseek(f, 0, SEEK_END); total = ftell(f) - 270; fclose(f); }
    }
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

    /* ---- the sequencer -------------------------------------------------
     * DPlayFile previews one sample file. It is not how a song gets played,
     * and handing it DINTRO.PXD - which is eJay's own encoded format, not PCM,
     * as its near-zero autocorrelation at every plausible width and offset
     * says - gets you the bytes rendered as samples. Which is static.
     *
     * The song path is the sequencer, and Dancejay's start-up runs it in this
     * order:
     *
     *     ASetPfad(dir) -> AStop() -> AMitte(0,0) -> RWaveParam(60, 0x6666)
     *       -> ASetFader(0,0) -> APlay(0,0,0,0,0, &status, file, 0,0,0,0, 0x100)
     *
     * and its play button then does ASetFader(0,0) -> AStart(0xA17FC0) and
     * pumps ATimer. 0xA17FC0 is 10,584,000 - four minutes at 44,100 - so
     * AStart is being told how long the arrangement is, not a magic number.
     *
     * The status word starts at 99 and the engine writes progress into it,
     * which is how the loader knows the sample is in. */
    fn_i_v   AStop      = (fn_i_v)   GetProcAddress(g_eng, "AStop");
    fn_i_ii  AMitte     = (fn_i_ii)  GetProcAddress(g_eng, "AMitte");
    fn_i_ii  RWaveParam = (fn_i_ii)  GetProcAddress(g_eng, "RWaveParam");
    fn_i_ii  ASetFader  = (fn_i_ii)  GetProcAddress(g_eng, "ASetFader");
    fn_i_p   ASetPfad   = (fn_i_p)   GetProcAddress(g_eng, "ASetPfad");
    fn_aplay APlay      = (fn_aplay) GetProcAddress(g_eng, "APlay");
    fn_i_i   AGetTime   = (fn_i_i)   GetProcAddress(g_eng, "AGetTime");
    fn_i_i   AStart     = (fn_i_i)   GetProcAddress(g_eng, "AStart");
    static short status = 0x63;
    if (seq) {
        /* Dancejay builds both of these by concatenating a directory global
         * with a name, so the directory carries its own trailing separator and
         * the name APlay is given is absolute. Neither mistake is reported: the
         * placed sample simply never loads and the status word the caller
         * handed over stays at the 99 it was set to. */
        char dir[MAX_PATH], full[MAX_PATH];
        GetCurrentDirectoryA(sizeof(dir), dir);
        size_t dl = strlen(dir);
        if (dl && dir[dl - 1] != '\\') { dir[dl] = '\\'; dir[dl + 1] = 0; }
        /* Leave an already-absolute name alone; concatenating onto it makes
         * "C:\a\C:\b", which the engine opens exactly as enthusiastically as
         * it opens anything else - and fails. */
        if (playfile[0] && playfile[1] == ':')
            snprintf(full, sizeof(full), "%s", playfile);
        else
            snprintf(full, sizeof(full), "%s%s", dir, playfile);

        if (ASetPfad)   printf("  ASetPfad(%s)\n", dir), ASetPfad(dir);
        if (AStop)      AStop();
        if (AMitte)     AMitte(0, 0);
        if (RWaveParam) RWaveParam(0x3c, 0x6666);
        if (ASetFader)  ASetFader(0, 0);
        if (APlay && g_lib_n && song) {
            /* One APlay per placed sample. Argument 8 is the track, 9 the start
             * position in samples at 44,100 - AStart's 0xA17FC0 is four minutes
             * in the same unit - and 6 is a status word the engine writes into.
             * A bar at 120 BPM is 88,200 of them. */
            static short st[64];
            int placed = 0;
            for (int i = 0; i < g_song_n && i < 64; i++) {
                const SLOT *sl = &g_song[i];
                if (sl->lib < 0 || sl->lib >= g_lib_n) continue;
                st[i] = 0x63;
                APlay(0, 0, 0, 0, 0, &st[i], g_lib[sl->lib].path,
                      sl->lane, sl->bar * 88200, 0, 0, 0x100);
                placed++;
            }
            printf("  APlay x%d across %d tracks\n", placed, g_song_n);
        } else if (APlay) {
            int r = APlay(0, 0, 0, 0, 0, &status, full, 0, 0, 0, 0, 0x100);
            printf("  APlay(%s) -> %d, status %d\n", full, r, status);
        }
        peek("after APlay");
        /* The intro function's own order: AFenster, then DStart, then AStart -
         * DStart is not only the sample-preview path's business. */
        {
            fn_i_i DStart = (fn_i_i) GetProcAddress(g_eng, "DStart");
            if (AFenster) AFenster((int)(INT_PTR)wnd);
            if (DStart)   printf("  DStart(0)                  -> %d\n", DStart(0));
        }
        if (ASetFader)  ASetFader(0, 0);
        if (AStart)     printf("  AStart(0xA17FC0)           -> %d\n", AStart(0xA17FC0));
        peek("after AStart");
    }

    float peak = 0.0f;
    static float env[600];
    int env_n = 0;
    double played = 0.0;   /* engine bytes from samples that already finished */
    DWORD t0 = GetTickCount();
    printf("\n  running %d ticks ...\n", ticks);
    /* ATimer is the stub; RTimer is the pump. RTimer is the function the
     * engine's own audio thread calls when it is allowed to, and it is what
     * arms the mixer - without it the mixer tick bails on a disarmed flag and
     * DirectSound plays an unfilled buffer, which is loud static. Dancejay
     * pumps RTimer once per frame in its play loop; so does this. */
    fn_i_v RTimer = (fn_i_v) GetProcAddress(g_eng, "RTimer");
    for (int i = 0; i < ticks; i++) {
        if (seq && RTimer) RTimer();
        if (seq) mute_unless_mixing(*(short *)((char *)g_eng + 0x3AC68));
        if (ATimer) ATimer();
        if (g_dplayupd) g_dplayupd();
        /* Refresh's argument is a millisecond timestamp into the intro, not a
         * page number: the DLL compares it against 0xd48, 0xfb9, 0x1770, 0x1ac2,
         * 0x1c75, 0x50dc, 0x5d8e and 0x7148, so the sequence runs for 29
         * seconds. Feeding it real elapsed time is what Dancejay's form timer
         * does; a synthetic i*16 runs the animation at whatever rate the host
         * manages instead. */
        if (main_screen && seq && AGetTime)
            draw_cursor(wnd, (double)AGetTime(0) / (double)0xA17FC0);
        else if (main_screen && dplay && DGetZeit) {
            /* Sixteen bars at 120 BPM is 32 seconds, and DGetZeit is a byte
             * offset into a 44.1kHz 16-bit stereo stream, so the sweep is the
             * engine's own clock in musical units rather than a wall timer
             * that happens to agree. Retrigger the sample when the engine says
             * it has finished, and keep the bytes it already played. */
            if (DCheck && !DCheck(chan)) {
                played += DGetZeit(chan);
                fn_i_pii DPF = (fn_i_pii) GetProcAddress(g_eng, "DPlayFile");
                if (DPF) DPF(playfile, 0, chan);
            }
            draw_cursor(wnd, (played + DGetZeit(chan)) / (176400.0 * 32.0));
        }
        int rc = 0;
        if (intro && Refresh) rc = Refresh((int)(GetTickCount() - t0));
        pump_messages();
        float p = meter_peak();
        if (p > peak) peak = p;
        if (env_n < (int)(sizeof(env) / sizeof(env[0]))) env[env_n++] = p;
        if (seq && i % 50 == 0)
            printf("    t=%5lums  AGetTime %8d  status %d  peak %.3f\n",
                   (unsigned long)(GetTickCount() - t0),
                   AGetTime ? AGetTime(0) : -1, status, peak);
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
    /* Keep the envelope, not just the maximum. Static sits flat near full
     * scale; a loop has a beat in it, and the difference is visible in one
     * column of asterisks without anyone having to listen to it first. */
    if (env_n > 8) {
        printf("  envelope, one row per 8 ticks (128ms):\n");
        for (int k = 0; k + 8 <= env_n; k += 8) {
            float m = 0;
            for (int j = 0; j < 8; j++) if (env[k + j] > m) m = env[k + j];
            printf("    %5.3f |", m);
            for (int j = 0; j < (int)(m * 40.0f + 0.5f); j++) putchar('#');
            putchar('\n');
        }
    }
    if (dplay && DCheck) printf("  DPlayFileCheck(%d) at end   -> %d\n", chan, DCheck(chan));
    if (dplay) {
        /* Zeit = time. A number that advanced with the wall clock is the
         * difference between "the call was accepted" and "the engine is
         * actually streaming the file". */
        if (DGetZeit) printf("  DGetZeit(%d) at end         -> %d\n", chan, DGetZeit(chan));
    }

    capture(wnd, shot);
    printf("  done\n");
    FreeLibrary(g_eng);
    if (g_gfx) FreeLibrary(g_gfx);
    return 0;
}
