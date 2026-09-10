/* ejay2.c - drive Dance eJay 2's shipped engine and graphics DLLs directly.
 *
 * PXD32D4.DLL (audio) and PXD32CL1.DLL (graphics) are native 32-bit code whose
 * imports are all DLLs Windows still ships, so they load and run as they are.
 * Nothing here is recompiled: this is the hybrid boundary from the other
 * direction, and its job is to establish the call protocol against real code
 * before Dancejay.exe gets lifted into that seat.
 *
 * The sequence is the one that made the 1997 engine play, and 27 of these
 * exports carry the same names in both versions because they are the same
 * source tree ported:
 *
 *     AInit -> ADevice -> AStart -> InterStart, then ATimer on a clock
 *
 * ADevice before AStart is the part that matters; without it no device opens.
 *
 * Volume is pinned low on purpose - a bring-up run queues whatever the mixer
 * happens to hold.
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <mmsystem.h>

#define ENGINE "PXD32D4.DLL"
#define GFXDLL "PXD32CL1.DLL"

/* Win16 PASCAL became __stdcall in the 32-bit port; the exports are
 * undecorated because they come from a .DEF file.
 *
 * The argument sizes are not guesses: a __stdcall callee ends `ret N`, so N is
 * read straight out of the DLL. That matters more than it sounds - AInit and
 * DPlayFile take NO arguments, and passing one leaks four bytes of stack per
 * call because the callee never pops it. Which is how the first version of
 * this host segfaulted. */
typedef int  (__stdcall *fn_i_i)(int);
typedef int  (__stdcall *fn_i_v)(void);
typedef int  (__stdcall *fn_i_ii)(int, int);
typedef int  (__stdcall *fn_i_7)(int, int, int, int, int, int, int);
typedef int  (__stdcall *fn_i_p)(void *);

static HMODULE g_eng, g_gfx;
static fn_i_v  g_dplayupd;
static int     g_quiet;

static FARPROC need(HMODULE h, const char *name)
{
    FARPROC p = GetProcAddress(h, name);
    if (!p) printf("  ! %s not found\n", name);
    return p;
}

/* ---- the window the graphics DLL draws into ---------------------------- */
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
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
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
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

/* Pin every wave-out device low. The engine sets its own level and a bring-up
 * run can queue anything, so this is a seatbelt rather than a preference. */
static void quiet_the_devices(int percent)
{
    UINT n = waveOutGetNumDevs();
    DWORD one = (DWORD)((65535.0 * percent) / 100.0);
    DWORD both = (one & 0xFFFF) | (one << 16);
    for (UINT i = 0; i < n; i++) waveOutSetVolume((HWAVEOUT)(UINT_PTR)i, both);
    printf("  volume pinned to %d%% across %u device(s)\n", percent, n);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* a crash must not eat the trail */
    int ticks = 200, volume = 20, want_gfx = 1, dplay = 0;
    const char *playfile = "DINTRO.PXD";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ticks") && i + 1 < argc) ticks = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--volume") && i + 1 < argc) volume = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-gfx")) want_gfx = 0;
        else if (!strcmp(argv[i], "--quiet")) g_quiet = 1;
        else if (!strcmp(argv[i], "--play") && i + 1 < argc) { playfile = argv[++i]; dplay = 1; }
        else if (!strcmp(argv[i], "--dplay")) dplay = 1;
    }

    printf("Dance eJay 2 - driving the 1999 DLLs directly\n\n");
    g_eng = LoadLibraryA(ENGINE);
    g_gfx = LoadLibraryA(GFXDLL);
    if (!g_eng) { printf("cannot load %s (%lu)\n", ENGINE, GetLastError()); return 1; }
    printf("  %s at %p\n", ENGINE, (void *)g_eng);
    if (g_gfx) printf("  %s at %p\n", GFXDLL, (void *)g_gfx);

    HWND wnd = make_window(640, 480);
    printf("  window %p\n", wnd);

    fn_i_v  AInit      = (fn_i_v) need(g_eng, "AInit");      /* ret 0  */
    fn_i_i  ADevice    = (fn_i_i) need(g_eng, "ADevice");
    fn_i_i  AStart     = (fn_i_i) need(g_eng, "AStart");
    fn_i_i  InterStart = (fn_i_i) need(g_eng, "InterStart");
    fn_i_v  ATimer     = (fn_i_v) need(g_eng, "ATimer");
    fn_i_i  AFenster   = (fn_i_i) GetProcAddress(g_eng, "AFenster");
    fn_i_i  ALautSet   = (fn_i_i) GetProcAddress(g_eng, "ALautSet");

    /* AFenster is German for window: hand the engine ours before anything. */
    if (AFenster) printf("  AFenster(hwnd) -> %d\n", AFenster((int)(INT_PTR)wnd));

    if (want_gfx && g_gfx) {
        fn_i_i SetActive = (fn_i_i) GetProcAddress(g_gfx, "GFX_SetActiveWindow");
        if (SetActive) printf("  GFX_SetActiveWindow(hwnd) -> %d\n",
                              SetActive((int)(INT_PTR)wnd));
    }

    if (AInit)   printf("  AInit()        -> %d\n", AInit());
    if (ADevice) printf("  ADevice(0)    -> %d\n", ADevice(0));
    quiet_the_devices(volume);
    if (ALautSet) printf("  ALautSet      -> %d\n", ALautSet(volume));
    if (AStart)  printf("  AStart(2e6)   -> %d\n", AStart(2000000));
    if (InterStart) printf("  InterStart(0) -> %d\n", InterStart(0));

    /* The 1999 engine has a family the 1997 one did not: DStart, DPlayFile,
     * DPlayUpdate, DPlayFileCheck, DGetZeit. Direct file playback - which is
     * how eJay 2 previews a sample, and a far shorter path to sound than the
     * sequencer was. */
    if (dplay) {
        fn_i_v DStart    = (fn_i_v) GetProcAddress(g_eng, "DStart");
        fn_i_v DPlayFile = (fn_i_v) GetProcAddress(g_eng, "DPlayFile");  /* ret 0 */
        fn_i_v DCheck    = (fn_i_v) GetProcAddress(g_eng, "DPlayFileCheck");
        fn_i_v DGetZeit  = (fn_i_v) GetProcAddress(g_eng, "DGetZeit");
        g_dplayupd       = (fn_i_v) GetProcAddress(g_eng, "DPlayUpdate");
        if (DStart)    printf("  DStart()       -> %d\n", DStart());
        /* DPlayFile takes nothing: the path goes over separately with
         * ASetPfad (Pfad = path) before the play call. */
        fn_i_p ASetPfad = (fn_i_p) GetProcAddress(g_eng, "ASetPfad");
        if (ASetPfad)  printf("  ASetPfad(%s)   -> %d\n", playfile,
                              ASetPfad((void *)playfile));
        if (DPlayFile) printf("  DPlayFile()    -> %d\n", DPlayFile());
        for (int k = 0; k < 25 && g_dplayupd; k++) { g_dplayupd(); Sleep(20); }
        if (DCheck)   printf("  DPlayFileCheck -> %d\n", DCheck());
        if (DGetZeit) printf("  DGetZeit       -> %d\n", DGetZeit());
    }

    printf("\n  running %d ticks ...\n", ticks);
    for (int i = 0; i < ticks; i++) {
        if (ATimer) ATimer();
        if (g_dplayupd) g_dplayupd();
        pump_messages();
        Sleep(16);
    }

    printf("  done\n");
    FreeLibrary(g_eng);
    if (g_gfx) FreeLibrary(g_gfx);
    return 0;
}
