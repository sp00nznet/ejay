/* win32.c - the platform layer on Windows, where the title's DLLs run as
 * themselves: a real window, the real message pump, and the endpoint meter
 * and session mute that keep a bring-up run quiet. host/posix.c is the same
 * layer for Linux and macOS. */
#include <stdio.h>
#include "plat.h"
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <audiopolicy.h>

HWND plat_window(int cx, int cy, const char *title, wndproc_t proc)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    /* No background brush: PXD32CL1 GetDC()s this window and blits straight
     * into it, so an erase on every invalidation would wipe its output. */
    wc.hbrBackground = NULL;
    wc.style = CS_DBLCLKS;      /* or WM_LBUTTONDBLCLK never arrives */
    wc.lpszClassName = "ejay2_host";
    RegisterClassA(&wc);
    RECT r = { 0, 0, cx, cy };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND h = CreateWindowExA(0, "ejay2_host", title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);
    HDC dc = GetDC(h);
    PatBlt(dc, 0, 0, cx, cy, BLACKNESS);
    ReleaseDC(h, dc);
    return h;
}

void plat_pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

int plat_headless(void) { return 0; }
const char *plat_lib_root(const char *libdir) { return libdir; }

const char *plat_cache(void)
{
    static char dir[MAX_PATH];
    if (!dir[0]) {
        DWORD n = GetTempPathA(sizeof(dir) - 8, dir);
        snprintf(dir + n, sizeof(dir) - n, "ejay2");
        CreateDirectoryA(dir, NULL);
    }
    return dir;
}

int plat_list(const char *dir, const char *ext, char names[][64], int max)
{
    char pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    int n = 0;
    snprintf(pat, sizeof(pat), "%s\\*%s", dir, ext);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do snprintf(names[n++], 64, "%s", fd.cFileName); while (n < max && FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

/* ---- did anything actually come out? ------------------------------------
 * PXD32D4 plays through DirectSound, so there is no write call to tap. The
 * endpoint meter (IAudioMeterInformation on the default render device)
 * reports the peak the hardware is actually fed, which settles "streaming"
 * versus "streaming silence" without capturing anything. */
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

float plat_peak(void)
{
    float v = 0.0f;
    if (g_meter) g_meter->lpVtbl->GetPeakValue(g_meter, &v);
    return v;
}

/* The engine only fills its DirectSound buffer while its mixer is armed; until
 * then DirectSound loops over uninitialised memory - full-scale noise that no
 * engine volume setting touches. So this process's audio session is held
 * muted for exactly as long as that flag is down, and its level clamped. */
void plat_audio_armed(int armed)
{
    static int last = -1;
    if (!g_sessionvol || armed == last) return;
    g_sessionvol->lpVtbl->SetMute(g_sessionvol, armed ? FALSE : TRUE, NULL);
    printf("  mixer %s -> audio %s\n", armed ? "armed" : "disarmed", armed ? "on" : "muted");
    last = armed;
}

void plat_audio_guard(int percent)
{
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    IAudioSessionManager *mgr = NULL;
    ISimpleAudioVolume *vol = NULL;
    if (FAILED(CoInitialize(NULL))) return;
    if (FAILED(CoCreateInstance(&k_MMDeviceEnumerator, NULL, CLSCTX_ALL, &k_IMMDeviceEnumerator, (void **)&en)))
        return;
    if (SUCCEEDED(en->lpVtbl->GetDefaultAudioEndpoint(en, eRender, eConsole, &dev))) {
        dev->lpVtbl->Activate(dev, &k_IAudioMeterInformation, CLSCTX_ALL, NULL, (void **)&g_meter);
        if (SUCCEEDED(dev->lpVtbl->Activate(dev, &k_IAudioSessionManager, CLSCTX_ALL, NULL, (void **)&mgr)) &&
            SUCCEEDED(mgr->lpVtbl->GetSimpleAudioVolume(mgr, NULL, FALSE, &vol))) {
            vol->lpVtbl->SetMasterVolume(vol, percent / 100.0f, NULL);
            vol->lpVtbl->SetMute(vol, TRUE, NULL);         /* until the mixer says otherwise */
            printf("  output                     -> %d%%, muted until the mixer arms\n", percent);
            g_sessionvol = vol;                            /* kept: the mute follows the mixer */
        }
    }
    if (mgr) mgr->lpVtbl->Release(mgr);
    if (dev) dev->lpVtbl->Release(dev);
    en->lpVtbl->Release(en);
}

/* A crash in a DLL nobody has source for is only useful if it says where:
 * the module and the offset into it, enough to find the function in the same
 * disassembly everything else came from. A vectored handler sees every
 * exception first-chance, before PXD32CL1's own __try blocks swallow it. */
static LONG CALLBACK first_chance(EXCEPTION_POINTERS *ep)
{
    static int shown;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != 0xE06D7363 && code != 0x406D1388 && shown < 8) {     /* C++ throws, thread names */
        void *pc = (void *)ep->ExceptionRecord->ExceptionAddress;
        HMODULE mod = NULL;
        char name[MAX_PATH] = "?";
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)pc, &mod);
        if (mod) GetModuleFileNameA(mod, name, sizeof(name));
        const char *tail = strrchr(name, '\\');
        printf("  !! exception %d: code %08lX  %s + %08lX\n", ++shown, (unsigned long)code,
               tail ? tail + 1 : name, mod ? (unsigned long)((char *)pc - (char *)mod) : 0ul);
        if (code == EXCEPTION_ACCESS_VIOLATION)
            printf("     %s address %p\n", ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                   (void *)ep->ExceptionRecord->ExceptionInformation[1]);
        fflush(stdout);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

int plat_open(int argc, char **argv)
{
    (void)argc; (void)argv;
    AddVectoredExceptionHandler(1, first_chance);
    return 0;
}
