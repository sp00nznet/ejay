/* posix.c - the platform layer for Linux and macOS: the title's DLLs, lifted
 * (tools/lift_ejay2.py), on pcrecomp's runtime/win32hle.
 *
 * win32hle answers every Win32 call the DLLs make, and this host's too (plat.h
 * routes them). What is here is the part a game exe would have brought: the
 * two images mapped and bound, the window, the screen it shows on, and the
 * guard that keeps an unarmed mixer's noise off the speakers.
 *
 * The guest's 4 GB is one block of this 64-bit process (win32hle's
 * guest_mem.c), so every pointer the DLLs see is an offset into it: plat.h's
 * GVA/HOSTP convert, and gmem/gcopy allocate there.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <limits.h>
#include <signal.h>
#include <libgen.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include "plat.h"
#include "pe_loader.h"
#include "host.h"

/* tools/lift_ejay2.py lifts both DLLs at these. */
#define ENGINE_BASE 0x10000000u
#define GFX_BASE    0x10100000u
#define SPAN        0x00100000u            /* each one's room: PXD32H4 is the largest, 0xB2000 */

static int  g_headless;
static char g_work[PATH_MAX];              /* work/<title>: engine.dll, gfx.dll */
static float g_volume = 0.2f;

/* ---- guest memory helpers ------------------------------------------------ */
gva gcopy(const void *p, size_t n)
{
    /* A ring: a copy lives until the ring comes round again, which is far
     * longer than any one call. Nothing the DLLs keep goes through here.
     * ponytail: 256 KB of ring; a DLL that kept a pointer past that would
     * read someone else's string. */
    enum { RING = 256 * 1024 };
    static uint8_t *ring;
    static size_t at;
    if (!ring) ring = (uint8_t *)hle_alloc(RING);
    size_t len = n;
    n = (n + 15) & ~(size_t)15;
    if (n > RING) hle_fatal("gcopy: %zu bytes is more than the ring", n);
    if (at + n > RING) at = 0;
    memcpy(ring + at, p, len);
    uint8_t *q = ring + at;
    at += n;
    return hle_va(q);
}

uint32_t xcall(gva fn, int n, const uint32_t *a)
{
    return fn ? hle_call_guest(fn, n, a) : 0;
}

gva api(const char *name)
{
    gva va = hle_resolve(name);
    if (!va) hle_fatal("win32hle has no %s", name);
    return va;
}

FILE *xfopen(const char *path, const char *mode)
{
    char host[PATH_MAX];
    hle_host_path(path, host, sizeof host);
    return fopen(host[0] ? host : path, mode);
}

void plat_sleep(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, NULL); }
uint32_t plat_ticks(void) { return hle_ticks_ms(); }
int plat_headless(void) { return g_headless; }

HBITMAP dib_section(HDC dc, const void *bmi, gva *bits)
{
    static uint32_t *out;
    if (!out) out = (uint32_t *)hle_alloc(4);
    *out = 0;
    HBITMAP b = W(CreateDIBSection, dc, GVA(bmi), 0, GVA(out), 0, 0);
    *bits = *out;
    return b;
}

/* ---- the lifted DLLs ------------------------------------------------------
 * Mapped where they were lifted, imports bound to win32hle (an import it
 * lacks becomes a stub that names itself when called), DllMain run. The
 * names the host asks for are the title's own (PXD32D4.DLL, PXD32CL1.DLL);
 * the images are work/<title>/engine.dll and gfx.dll, already rebased. */
gva load_dll(const char *name)
{
    const char *b = strrchr(name, '\\');
    b = b ? b + 1 : name;
    int is_gfx = !strcasecmp(b, "PXD32CL1.DLL");
    char file[PATH_MAX + 16], guest[64];
    snprintf(file, sizeof file, "%s/%s", g_work, is_gfx ? "gfx.dll" : "engine.dll");
    pe_image img;
    hle_vm_release(is_gfx ? GFX_BASE : ENGINE_BASE, SPAN);   /* held since plat_open */
    if (recomp_pe_map(file, &img) != 0) {
        fprintf(stderr, "cannot map %s: run setup.sh, which lifts the title's DLLs\n", file);
        return 0;
    }
    if (img.base != (is_gfx ? GFX_BASE : ENGINE_BASE))
        hle_fatal("%s is at 0x%08X, not where it was lifted", file, img.base);
    snprintf(guest, sizeof guest, "C:\\%s", b);
    hle_module_add(guest, img.base, img.resource_rva, img.resource_size, 0);
    int unresolved = recomp_pe_bind(&img, hle_resolve_or_stub);
    uint32_t args[3] = { img.base, 1, 0 };            /* DLL_PROCESS_ATTACH */
    uint32_t ok = img.entry ? hle_call_guest(img.entry, 3, args) : 1;
    fprintf(stderr, "[load] %s at 0x%08X, %d imports not implemented, DllMain -> %u\n",
            b, img.base, unresolved, ok);
    return img.base;
}

/* An export by name, from the image's export directory in guest memory. */
gva dll_proc(gva base, const char *name)
{
    uint32_t rva = MEM32(base + MEM32(base + 0x3C) + 0x78);
    if (!rva) return 0;
    uint32_t ed = base + rva, nfun = MEM32(ed + 20), nnames = MEM32(ed + 24);
    uint32_t funcs = base + MEM32(ed + 28), names = base + MEM32(ed + 32), ords = base + MEM32(ed + 36);
    for (uint32_t k = 0; k < nnames; k++)
        if (!strcmp((const char *)hle_ptr(base + MEM32(names + 4 * k)), name)) {
            uint32_t idx = MEM16(ords + 2 * k);
            return idx < nfun ? base + MEM32(funcs + 4 * idx) : 0;
        }
    return 0;
}

/* The lift's hook for a function the catalog had and the lift stubbed. */
void recomp_not_lifted(uint32_t va)
{
    fprintf(stderr, "\n[not-lifted] sub_%08X (called from 0x%08X)\n", va, g_cur_func);
    hle_exit(2);
}

/* ---- the window ----------------------------------------------------------
 * A 640x480 popup at the screen's origin, so its client area is the window
 * surface win32hle's GDI draws window DCs into, and that surface is what
 * screen.c shows, scaled to the SDL window. */
static wndproc_t g_proc;
static void wndproc_shim(void)                     /* (hwnd, msg, wParam, lParam), stdcall */
{
    uint32_t h = A32(0), m = A32(1), w = A32(2), l = A32(3);
    RET(g_proc(h, m, w, l), 4);
}
static const win32hle_shim g_shims[] = { { "ejay2_wndproc", wndproc_shim }, { 0, 0 } };

HWND plat_window(int w, int h, const char *title, wndproc_t proc)
{
    static uint32_t *px;
    px = (uint32_t *)calloc((size_t)w * h, 4);
    hle_gdi_window_surface((uint8_t *)px, w, h, w * 4, 32);
    hle_set_screen_size(w, h);
    if (hle_screen_open(title, 0, g_headless) != 0) hle_fatal("no screen: SDL could not open a window");
    g_proc = proc;
    uint32_t *wc = (uint32_t *)gmem(40);               /* WNDCLASSA */
    wc[0] = 8;                                         /* CS_DBLCLKS, or no WM_LBUTTONDBLCLK */
    wc[1] = hle_resolve("ejay2_wndproc");
    wc[9] = GSTR("ejay2_host");
    if (!W(RegisterClassA, GVA(wc))) hle_fatal("RegisterClassA failed");
    HWND wnd = W(CreateWindowExA, 0, GSTR("ejay2_host"), GSTR(title),
                 0x90000000u,                          /* WS_POPUP | WS_VISIBLE */
                 0, 0, (uint32_t)w, (uint32_t)h, 0, 0, 0, 0);
    HDC dc = GetDC(wnd);
    PatBlt(dc, 0, 0, w, h, BLACKNESS);
    ReleaseDC(wnd, dc);
    return wnd;
}

void plat_pump(void)
{
    static uint32_t *msg;
    if (!msg) msg = (uint32_t *)gmem(28);
    while (W(PeekMessageA, GVA(msg), 0, 0, 0, 1)) {      /* PM_REMOVE */
        if (msg[1] == 0x0012) { hle_exit(0); }          /* WM_QUIT: the window was closed */
        W(TranslateMessage, GVA(msg));
        W(DispatchMessageA, GVA(msg));
    }
    hle_screen_pump();
}

/* ---- audio ---------------------------------------------------------------
 * The engine only fills its DirectSound buffer while its mixer is armed; until
 * then the buffer is uninitialised and would play as full-scale noise. So the
 * output is held at zero until the host sees the arm flag. */
void plat_audio_guard(int percent)
{
    g_volume = percent / 100.0f;
    hle_dsound_set_master(0.0f);
    printf("  output                     -> %d%%, muted until the mixer arms\n", percent);
}
void plat_audio_armed(int armed)
{
    static int last = -1;
    if (armed == last) return;
    hle_dsound_set_master(armed ? g_volume : 0.0f);
    printf("  mixer %s -> audio %s\n", armed ? "armed" : "disarmed", armed ? "on" : "muted");
    last = armed;
}
float plat_peak(void) { return 0.0f; }   /* ponytail: no endpoint meter here; --onset is Windows-only */

static void on_fault(int sig)
{
    fprintf(stderr, "\n=== signal %d in lifted sub_%08X, last import %s ===\n", sig, g_cur_func,
            g_cur_import ? g_cur_import : "(none)");
    _exit(3);
}

/* Platform flags come out of argv here: --headless, --work DIR, --lib-drive. */
int plat_open(int argc, char **argv)
{
    char exe[PATH_MAX];
    g_work[0] = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless")) g_headless = 1;
        else if (!strcmp(argv[i], "--trace")) win32hle_trace = 1;   /* every Win32 call, by name */
        else if (!strcmp(argv[i], "--work") && i + 1 < argc) {
            if (!realpath(argv[i + 1], g_work)) { fprintf(stderr, "no folder %s\n", argv[i + 1]); return 1; }
        }
    }
    if (!g_work[0]) {                                  /* default: <repo>/work/<title>, beside build-<title>/ */
        if (!hle_exe_path(exe, sizeof exe)) { fprintf(stderr, "pass --work <repo>/work/<title>\n"); return 1; }
        char *dir = dirname(exe);
        snprintf(g_work, sizeof g_work, "%s/../work/%s", dir, EJAY_TITLE);
    }
    signal(SIGSEGV, on_fault);
    signal(SIGBUS, on_fault);
    signal(SIGILL, on_fault);
    signal(SIGFPE, on_fault);

    /* C: is the program folder, which is where this runs from: the DLLs load
     * GRAFIKA\..., K_640 and their palettes by relative path. */
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) return 1;
    hle_set_drive('C', cwd);
    win32hle_register(g_shims);
    recomp_host_init();
    /* Held from the start: the engine's DllMain allocates, and its heap would
     * otherwise land where the graphics DLL has to go. */
    if (!hle_vm_reserve(ENGINE_BASE, SPAN) || !hle_vm_reserve(GFX_BASE, SPAN))
        hle_fatal("cannot hold the DLLs' address ranges");
    return 0;
}

int plat_list(const char *dir, const char *ext, char names[][64], int max)
{
    char host[PATH_MAX];
    hle_host_path(dir, host, sizeof host);
    DIR *d = opendir(host[0] ? host : dir);
    int n = 0;
    size_t el = strlen(ext);
    for (struct dirent *e; d && n < max && (e = readdir(d)); ) {
        size_t l = strlen(e->d_name);
        if (l > el && !strcasecmp(e->d_name + l - el, ext)) snprintf(names[n++], 64, "%s", e->d_name);
    }
    if (d) closedir(d);
    return n;
}

/* A folder to write into: $XDG_CACHE_HOME/ejay2, or ~/.cache/ejay2 (on a Mac,
 * ~/Library/Caches/ejay2), as drive E:. */
const char *plat_cache(void)
{
    static int done;
    if (!done) {
        char dir[PATH_MAX];
        const char *x = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
#ifdef __APPLE__
        snprintf(dir, sizeof dir, "%s/Library/Caches", home ? home : "/tmp");
#else
        if (x && *x) snprintf(dir, sizeof dir, "%s", x);
        else snprintf(dir, sizeof dir, "%s/.cache", home ? home : "/tmp");
#endif
        mkdir(dir, 0755);
        strncat(dir, "/ejay2", sizeof dir - strlen(dir) - 1);
        mkdir(dir, 0755);
        hle_set_drive('E', dir);
        done = 1;
    }
    return "E:";
}

/* --lib: the library is wherever the user keeps it, so it gets a drive of
 * its own and the host and the engine both name it D:. */
const char *plat_lib_root(const char *libdir)
{
    char real[PATH_MAX];
    if (!realpath(libdir, real)) return libdir;
    hle_set_drive('D', real);
    return "D:";
}
