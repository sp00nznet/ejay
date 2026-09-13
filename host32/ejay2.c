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
#include <ctype.h>

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
typedef int (__stdcall *fn_setkey)(const char *, int, int, int, int, int,
                                   int, int, int, int, int);
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
/* set by the window procedure, acted on by the main loop */
static int g_dirty = 1;
static int g_place = -1;
/* the graphics DLL handles the grid is drawn with, and the engine call that
 * places a sample - both needed again whenever the arrangement changes */
static fn_zeich g_zeich;
static fn_aplay g_aplay;
static int g_griddc;
static int g_tex[3];
static double g_cursor_frac;
static fn_i_v  g_dplayupd;

static FARPROC need(HMODULE h, const char *name)
{
    FARPROC p = GetProcAddress(h, name);
    if (!p) printf("  ! %s not found\n", name);
    return p;
}

/* Defined below, with the browser it belongs to: returns 1 if it consumed
 * the message. */
static int browser_input(HWND h, UINT m, WPARAM w, LPARAM l);

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
    if (browser_input(h, m, w, l)) return 0;
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
    wc.style = CS_DBLCLKS;      /* or WM_LBUTTONDBLCLK never arrives */
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

/* Counting the GDI the graphics DLL does is the only way to tell a draw loop
 * that is too long from a wait that is too patient. */
static long g_n_bitblt, g_n_patblt, g_n_setpixel;
static BOOL (WINAPI *g_real_bitblt)(HDC, int, int, int, int, HDC, int, int, DWORD);
static BOOL (WINAPI *g_real_patblt)(HDC, int, int, int, int, DWORD);
static COLORREF (WINAPI *g_real_setpixel)(HDC, int, int, COLORREF);

static BOOL WINAPI count_bitblt(HDC d, int x, int y, int w, int h,
                                HDC sd, int sx, int sy, DWORD rop)
{ g_n_bitblt++; return g_real_bitblt(d, x, y, w, h, sd, sx, sy, rop); }

static BOOL WINAPI count_patblt(HDC d, int x, int y, int w, int h, DWORD rop)
{ g_n_patblt++; return g_real_patblt(d, x, y, w, h, rop); }

static COLORREF WINAPI count_setpixel(HDC d, int x, int y, COLORREF c)
{ g_n_setpixel++; return g_real_setpixel(d, x, y, c); }

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

/* ---- watching the graphics DLL's own state --------------------------------
 * The only reliable way to learn what one of these calls does is to photograph
 * its data before and after. Everything known about GFX_IntroSetKey came out
 * of doing it once; this makes it a routine. */
static int  *g_snap;
static DWORD g_snap_n;

static void snap_take(HMODULE mod)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((char *)mod + dos->e_lfanew);
    g_snap_n = nt->OptionalHeader.SizeOfImage;
    if (!g_snap) g_snap = (int *)malloc(g_snap_n);
    if (g_snap) memcpy(g_snap, mod, g_snap_n);
}

/* Report changed dwords, optionally only inside one window of the image. */
static void snap_diff(HMODULE mod, const char *what, DWORD lo, DWORD hi, int max)
{
    if (!g_snap) return;
    const char *b = (const char *)mod;
    int shown = 0;
    for (DWORD off = lo & ~3u; off + 4 <= hi && off + 4 <= g_snap_n; off += 4) {
        int a0 = g_snap[off / 4], a1 = *(const int *)(b + off);
        if (a0 == a1) continue;
        if (!shown) printf("  [%s] changed:\n", what);
        if (shown < max)
            printf("    +%06lX  %11d -> %-11d\n", (unsigned long)off, a0, a1);
        shown++;
    }
    if (!shown) printf("  [%s] changed nothing in that range\n", what);
    else if (shown > max) printf("    ... and %d more\n", shown - max);
    snap_take(mod);
}

/* Peek at the engine's own globals. Their addresses come out of the
 * disassembly, and watching them beats guessing which call quietly did
 * nothing: 0x4342C is the handshake AStart spins on, 0x43394 / 0x4339C /
 * 0x433A4 are the three gates AGetTime checks before it will report a
 * position, and the word at track0+0x6A counts the samples APlay has placed. */
/* The track records live at base+0x429C8, 0x84 apart. +0x6A is how many
 * entries APlay has appended, +0x68 the cursor the mixer compares against it,
 * +0x60 the word AStart's worker resets, +0x64 the entry array - and each entry
 * is 0x108 bytes with start at +0 and end at +4. Reading them back is the only
 * way to see what the engine thinks it has been given. */
static void tracks(const char *when, int n)
{
    const char *b = (const char *)g_eng;
    printf("  [%s] transport %d of %d\n", when,
           *(int *)(b + 0x3A0C0), *(int *)(b + 0x3B150));
    for (int t = 0; t < n; t++) {
        const char *tr = b + 0x429C8 + t * 0x84;
        int count = *(short *)(tr + 0x6A);
        if (!count) continue;
        printf("    track %d: count %d cursor %d w60 %d entries %p\n", t, count,
               *(short *)(tr + 0x68), *(short *)(tr + 0x60),
               *(void **)(tr + 0x64));
        const char *ents = *(const char **)(tr + 0x64);
        for (int e = 0; ents && e < count && e < 4; e++)
            printf("      entry %d: start %d end %d id %d\n", e,
                   *(int *)(ents + e * 0x108), *(int *)(ents + e * 0x108 + 4),
                   *(short *)(ents + e * 0x108 + 0x10));
    }
}

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

/* ---- the sample library ---------------------------------------------------
 * eJay does not walk its own disc looking for samples - it ships an index, and
 * decoding it is the difference between a browser with names in it and a
 * browser that works.
 *
 *   DMACHINE\PXD.TXT   18 quoted numbers, then four quoted fields per sample:
 *                      size, length in bars, name line 1, name line 2.
 *                      The 18 numbers are nine (start, count) pairs - the nine
 *                      sound groups the category buttons switch between, and
 *                      they add up to exactly the 1,352 records that follow.
 *   DMACHINE\MAX.TXT   one quoted path per sample, same order: "ba\aaaf.pxd".
 *                      MIN.TXT is the same list for a minimal install.
 *
 * eJay 2's library is on its second disc. This is eJay 1's, and the 1999 engine
 * plays it without being asked to do anything special about it.
 */
typedef struct {
    char path[MAX_PATH];
    char l1[28], l2[28];
    int  bars, len;             /* len is the decoded length in 44.1k samples */
} SAMPLE;

#define MAX_GROUPS 12
static SAMPLE g_lib[1400];
static int    g_lib_n;
static int    g_grp_start[MAX_GROUPS], g_grp_count[MAX_GROUPS], g_grp_n;

/* browser state */
static int g_group  = 0;    /* which sound group is showing   */
static int g_scroll = 0;    /* first visible row within it    */
static int g_sel    = -1;   /* absolute index of the selection */

/* Pull the next "quoted" field out of a buffer. */
static const char *next_field(const char *p, char *out, size_t n)
{
    while (*p && *p != '"') p++;
    if (!*p) return NULL;
    p++;
    size_t k = 0;
    while (*p && *p != '"') { if (k + 1 < n) out[k++] = *p; p++; }
    out[k] = 0;
    return *p ? p + 1 : p;
}

static char *slurp(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) == (size_t)n) b[n] = 0;
    else { free(b); b = NULL; }
    fclose(f);
    if (len) *len = n;
    return b;
}

/* The unit APlay's start and length arguments are in, measured rather than
 * guessed. Two facts settle it:
 *
 *  - the engine's transport counter advances 176,400 per second of playback -
 *    44,100 frames of 16-bit stereo, so its unit is an output byte, not a
 *    sample. (Measured against AGetTime, which does report milliseconds.)
 *  - Dance eJay's tempo is a fixed 140 BPM, so a 4/4 bar is 60*4/140 = 1.714
 *    seconds. The sample files agree: a tPxD header declares 151,200 samples
 *    for a two-bar loop, which is exactly two bars at that tempo.
 *
 * 1.714 seconds of output bytes is 302,400. The 88,200 this host used before was
 * 120 BPM in samples - wrong tempo and wrong unit, a factor of 3.4 too short,
 * which is why every block was cut off and the whole arrangement fired at once
 * in the first few seconds. */
#define BAR_UNITS 302400

/* ---- how long a sample actually is --------------------------------------
 * PXD.TXT's second field is read below as a bar count, and it is not reliable
 * enough to time an arrangement with. The sample file says it exactly: a tPxD
 * header is the tag, a Pascal-string name, a 0x54 byte, then the decoded length
 * as a byte count of 16-bit mono PCM - 151,200 for a one-bar loop and 37,800
 * for a one-beat hit, both exact at 140 BPM. The engine counts 16-bit stereo
 * output bytes, so twice that is the length APlay wants. Bytes rather than
 * samples is what the measurement said: an entry told it ran for two bars fell
 * silent after one, every time. */
static int pxd_length(const char *path)
{
    unsigned char h[64];
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(h, 1, sizeof(h), f);
    fclose(f);
    if (n < 16 || memcmp(h, "tPxD", 4)) return 0;
    unsigned k = 5u + h[4];                 /* past the tag and the name */
    if (k + 5 > n || h[k] != 0x54) return 0;
    return (int)(h[k+1] | (h[k+2] << 8) | (h[k+3] << 16) | (h[k+4] << 24));
}

/* `root` is the folder holding the two-letter sample directories; the index
 * lives in DMACHINE beside them. */
static int load_index(const char *root)
{
    char p1[MAX_PATH], p2[MAX_PATH], fld[256];
    snprintf(p1, sizeof(p1), "%s\\DMACHINE\\PXD.TXT", root);
    snprintf(p2, sizeof(p2), "%s\\DMACHINE\\MAX.TXT", root);
    char *idx = slurp(p1, NULL), *files = slurp(p2, NULL);
    if (!idx || !files) { free(idx); free(files); return 0; }

    const char *q = idx;
    for (int i = 0; i < MAX_GROUPS && q; i++) {
        char a[64], b[64];
        q = next_field(q, a, sizeof(a));
        if (!q) break;
        q = next_field(q, b, sizeof(b));
        if (!q) break;
        g_grp_start[i] = atoi(a);
        g_grp_count[i] = atoi(b);
        g_grp_n = i + 1;
        /* Nine pairs, and the ninth ends where the records begin - so stop
         * when the starts stop climbing rather than trusting a count. */
        if (i && g_grp_start[i] <= g_grp_start[i - 1]) { g_grp_n = i; break; }
        if (g_grp_n == 9) break;
    }

    const char *fq = files;
    while (q && g_lib_n < (int)(sizeof(g_lib) / sizeof(g_lib[0]))) {
        SAMPLE *sm = &g_lib[g_lib_n];
        char size[64], bars[64];
        q = next_field(q, size, sizeof(size));  if (!q) break;
        q = next_field(q, bars, sizeof(bars));  if (!q) break;
        q = next_field(q, fld, sizeof(fld));    if (!q) break;
        snprintf(sm->l1, sizeof(sm->l1), "%s", fld);
        q = next_field(q, fld, sizeof(fld));    if (!q) break;
        snprintf(sm->l2, sizeof(sm->l2), "%s", fld);
        fq = next_field(fq, fld, sizeof(fld));
        if (!fq) break;
        snprintf(sm->path, sizeof(sm->path), "%s\\%s", root, fld);
        sm->len  = pxd_length(sm->path);
        /* Whole bars, rounded up: a block occupies bars on eJay's grid, and the
         * end an entry is given is also when the lane is free again. Rounding
         * down would cut the tail off every loop that is not exactly on the
         * bar. A quarter-bar hit still occupies one. */
        sm->bars = sm->len ? (sm->len * 2 + BAR_UNITS - 1) / BAR_UNITS : atoi(bars);
        if (sm->bars < 1 || sm->bars > 16) sm->bars = 1;
        g_lib_n++;
    }
    free(idx); free(files);
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
static SLOT g_song[64];
static int  g_song_n;

/* The arrangement as written rather than as indexed: a lane, a bar, and which
 * sound group to take the sample from. Resolving it after the index is read
 * means the blocks get the samples' real lengths, and picking across groups
 * means the demo is a beat with something over it rather than nine variations
 * of the same pad - which is what indices 0..7 of group 0 happen to be, and why
 * it was barely audible. */
typedef struct { int lane, bar, grp, idx; } SEED;
static const SEED g_seed[] = {
    { 0,  0, 3, 0 }, { 0,  8, 3, 0 },          /* "lasting / complete", 4 bars */
    { 1,  2, 0, 0 }, { 1,  6, 0, 1 },          /* "Grp. 1" loops, 2 bars       */
    { 1, 10, 0, 0 }, { 1, 14, 0, 1 },
    { 2,  0, 5, 2 }, { 2,  4, 5, 2 },          /* claps on the four            */
    { 2,  8, 5, 2 }, { 2, 12, 5, 2 },
    { 3,  4, 6, 1 },                           /* "Happy Hyp" pad, 8 bars      */
    { 4,  8, 8, 0 }, { 4, 12, 8, 2 },          /* a vocal, and an answer       */
};

static void build_song(void)
{
    g_song_n = 0;
    for (int i = 0; i < (int)(sizeof(g_seed) / sizeof(g_seed[0])); i++) {
        const SEED *sd = &g_seed[i];
        if (sd->grp >= g_grp_n || sd->idx >= g_grp_count[sd->grp]) continue;
        int lib = g_grp_start[sd->grp] + sd->idx;
        if (lib >= g_lib_n) continue;
        SLOT *sl = &g_song[g_song_n++];
        sl->lane = sd->lane; sl->bar = sd->bar;
        sl->lib = lib; sl->bars = g_lib[lib].bars;
    }
}

/* ---- eJay's own control layout --------------------------------------------
 * `K_640` and its siblings are the coordinate table: a control name, then ten
 * numbers.
 *
 *     name   dx dy   s1x s1y   s2x s2y   s3x s3y   w h
 *
 * `dx,dy` is where the control sits on screen and `w,h` how big it is; the
 * three source pairs are its states cut out of `EJAY02` - normal, rolled over,
 * pressed - which is what SEITEN means by `RollOverButton`. A two-state control
 * leaves the third pair at 0,0.
 *
 * Every number is in a 1280x960 space, so the 640x480 art set halves them.
 * K_640, K_800 and K_1280 are near-identical files precisely because the
 * numbers do not depend on the resolution - only the rounding does. Reading
 * that wrongly puts a 63-pixel button at x 1216 on a 640-pixel screen, which is
 * how it looked like nonsense the first time.
 */
#define KSCALE 2

typedef struct {
    char name[32];
    int  x, y, w, h;
    int  sx[3], sy[3];
    int  raw[10];        /* unscaled, for the calls that want them as written */
} KCTRL;

static KCTRL g_ctrl[640];
static int   g_ctrl_n;

static int load_keys(const char *path)
{
    char *buf = slurp(path, NULL);
    if (!buf) return 0;
    char *p = buf;
    while (*p && g_ctrl_n < (int)(sizeof(g_ctrl) / sizeof(g_ctrl[0]))) {
        /* a name line: starts with a letter */
        while (*p && (*p == '\r' || *p == '\n' || *p == ' ')) p++;
        if (!*p) break;
        if (!isalpha((unsigned char)*p)) {          /* stray number, skip it */
            while (*p && *p != '\n') p++;
            continue;
        }
        char name[32];
        int k = 0;
        while (*p && *p != '\r' && *p != '\n') {
            if (k + 1 < (int)sizeof(name) && *p != ' ') name[k++] = *p;
            p++;
        }
        name[k] = 0;

        int n[10], got = 0;
        char *save = p;
        while (got < 10 && *p) {
            while (*p == '\r' || *p == '\n' || *p == ' ') p++;
            if (!*p) break;
            if (!isdigit((unsigned char)*p) && *p != '-') break;
            n[got++] = atoi(p);
            while (*p && *p != '\r' && *p != '\n') p++;
        }
        if (got < 10) { p = save; continue; }

        KCTRL *c = &g_ctrl[g_ctrl_n++];
        snprintf(c->name, sizeof(c->name), "%s", name);
        c->x = n[0] / KSCALE; c->y = n[1] / KSCALE;
        c->w = n[8] / KSCALE; c->h = n[9] / KSCALE;
        for (int i = 0; i < 3; i++) {
            c->sx[i] = n[2 + i * 2] / KSCALE;
            c->sy[i] = n[3 + i * 2] / KSCALE;
        }
        memcpy(c->raw, n, sizeof(c->raw));
    }
    free(buf);
    return g_ctrl_n;
}

static const KCTRL *find_ctrl(const char *name)
{
    for (int i = 0; i < g_ctrl_n; i++)
        if (!strcmp(g_ctrl[i].name, name)) return &g_ctrl[i];
    return NULL;
}

/* Which control is under the pointer? Only the ones we know how to draw. */
static int ctrl_at(int x, int y)
{
    for (int i = 0; i < g_ctrl_n; i++) {
        const KCTRL *c = &g_ctrl[i];
        if (c->name[0] != 'B' || c->name[1] != '_') continue;
        if (!c->sx[1] && !c->sy[1]) continue;        /* no rollover state */
        if (x >= c->x && x < c->x + c->w && y >= c->y && y < c->y + c->h)
            return i;
    }
    return -1;
}

/* Blit one state of one control from the EJAY02 sheet. */
static void draw_ctrl(HDC dst, const KCTRL *c, int state)
{
    if (!c || !g_sheet.hdc) return;
    if (state && !c->sx[state] && !c->sy[state]) state = 0;
    if (state == 0) {
        /* state 0 is simply what the chrome already has */
        if (g_screen.hdc)
            BitBlt(dst, c->x, c->y, c->w, c->h,
                   (HDC)(INT_PTR)g_screen.hdc, c->x, c->y, SRCCOPY);
        return;
    }
    BitBlt(dst, c->x, c->y, c->w, c->h, (HDC)(INT_PTR)g_sheet.hdc,
           c->sx[state], c->sy[state], SRCCOPY);
}

/* ---- the browser ---------------------------------------------------------
 * The bottom-centre panel is `G_SAMPLE_WINDOW` in eJay's layout table, with
 * `K_SAMPLE_VSCROLL` down its right edge and twelve `B_GRUPPE_*` buttons either
 * side of it - six left (Loop, Drum, Bass, Guitar, Seq, Layer) and six right
 * (Rap, Voice, Effect, Xtra, GrooveG, Wave). eJay 1's library has nine sound
 * groups, so nine of the twelve do something and three stay dark.
 *
 * Geometry measured off EJAY01A: the panel at x 176..539, y 366..472; the
 * button lozenges 49 wide at x 112 and x 569, 16 tall, on a 22-pixel pitch
 * from y 346.
 */
#define ROW_H     11
#define BTN_W     49
#define BTN_H     16
#define BTN_LX   112
#define BTN_RX   569
#define BTN_Y0   346
#define BTN_PITCH 22
#define SCROLL_X 543

/* The panel is `G_SAMPLE_WINDOW` and the strip beside it `K_SAMPLE_VSCROLL`,
 * both in eJay's own table - x 178..537, y 380..473 once halved. Taking them
 * from the table rather than from a ruler is what stops the first row being
 * drawn over the transport bar above it. */
static RECT g_browse = { BROWSE_X0, BROWSE_Y0, BROWSE_X1, BROWSE_Y1 };
static int  g_scroll_x = SCROLL_X, g_scroll_w = 9;

static int browser_rows(void);      /* defined just below */

static void browser_geometry(void)
{
    const KCTRL *c = find_ctrl("G_SAMPLE_WINDOW");
    if (c && c->w > 0 && c->h > 0) {
        g_browse.left = c->x; g_browse.top = c->y;
        g_browse.right = c->x + c->w; g_browse.bottom = c->y + c->h;
    }
    c = find_ctrl("K_SAMPLE_VSCROLL");
    if (c && c->w > 0) { g_scroll_x = c->x; g_scroll_w = c->w; }
    printf("  browser panel              -> x %d..%d, y %d..%d, %d rows\n",
           (int)g_browse.left, (int)g_browse.right,
           (int)g_browse.top, (int)g_browse.bottom, browser_rows());
}

/* G_SAMPLE_WINDOW is the panel including its bezel, and text drawn at its very
 * top and bottom edges sits on the frame. Inset by the bezel. */
#define BEZEL_T 7
#define BEZEL_B 6
#define BEZEL_L 6

static int browser_rows(void)
{
    return (int)(g_browse.bottom - g_browse.top - BEZEL_T - BEZEL_B) / ROW_H;
}

/* Draw every placed sample into the lanes. Called once at start-up and again
 * whenever the arrangement gains a block, so the grid always shows the list
 * the engine is playing. */
static void draw_blocks(HDC dst)
{
    if (!g_zeich || !g_griddc || !g_screen.hdc) return;
    const int bar = (GRID_X1 - GRID_X0) / 16;
    BitBlt(dst, GRID_X0, GRID_Y0, GRID_X1 - GRID_X0, GRID_Y1 - GRID_Y0,
           (HDC)(INT_PTR)g_screen.hdc, GRID_X0, GRID_Y0, SRCCOPY);
    for (int i = 0; i < g_song_n; i++) {
        const SLOT *sl = &g_song[i];
        int w = sl->bars * bar, style = g_tex[i % 3];
        const SAMPLE *sm = (sl->lib >= 0 && sl->lib < g_lib_n) ? &g_lib[sl->lib] : NULL;
        if (!style || w < 4) continue;
        g_zeich(style, 0, w, sm ? sm->l1 : "Sample", sm ? sm->l2 : "", 0, 0);
        BitBlt(dst, GRID_X0 + sl->bar * bar, GRID_Y0 + sl->lane * 18 + 1,
               w - 2, 16, (HDC)(INT_PTR)g_griddc, 0, 0, SRCCOPY);
    }
}

/* Which category button is under the pointer, or -1. Buttons run down the left
 * column first, then the right, which is the order SEITEN lists them in. */
static int hit_group(int x, int y)
{
    for (int i = 0; i < 12; i++) {
        int bx = (i < 6) ? BTN_LX : BTN_RX;
        int by = BTN_Y0 + (i % 6) * BTN_PITCH;
        if (x >= bx && x < bx + BTN_W && y >= by && y < by + BTN_H) return i;
    }
    return -1;
}

static void draw_browser(HDC dst)
{
    if (!g_lib_n) return;
    int gs = g_grp_start[g_group], gc = g_grp_count[g_group];
    int rows = browser_rows();
    if (g_scroll > gc - rows) g_scroll = gc - rows;
    if (g_scroll < 0) g_scroll = 0;
    if (!dst || !g_screen.hdc) return;   /* clamp only, for the selftest */

    /* Repaint the panel from the chrome first, so a scroll does not smear. */
    BitBlt(dst, (int)g_browse.left, (int)g_browse.top, (int)g_browse.right - (int)g_browse.left, (int)g_browse.bottom - (int)g_browse.top,
           (HDC)(INT_PTR)g_screen.hdc, (int)g_browse.left, (int)g_browse.top, SRCCOPY);

    HFONT fnt = CreateFontA(-10, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                            0, 0, DEFAULT_QUALITY, 0, "Small Fonts");
    HFONT old = (HFONT)SelectObject(dst, fnt);
    SetBkMode(dst, TRANSPARENT);
    /* Clip to the panel: a row that does not fit belongs nowhere, and without
     * this the top one is drawn over the transport bar above it. */
    HRGN clip = CreateRectRgn((int)g_browse.left + BEZEL_L,
                              (int)g_browse.top + BEZEL_T - 1,
                              (int)g_browse.right - BEZEL_L,
                              (int)g_browse.bottom - BEZEL_B + 1);
    SelectClipRgn(dst, clip);

    for (int r = 0; r < rows && g_scroll + r < gc; r++) {
        int idx = gs + g_scroll + r;
        int y = (int)g_browse.top + BEZEL_T + r * ROW_H;
        const SAMPLE *sm = &g_lib[idx];
        if (idx == g_sel) {
            RECT sel = { (int)g_browse.left + 4, y - 1, (int)g_browse.right - 6, y + ROW_H - 1 };
            HBRUSH hb = CreateSolidBrush(RGB(40, 60, 150));
            FillRect(dst, &sel, hb);
            DeleteObject(hb);
        }
        SetTextColor(dst, idx == g_sel ? RGB(255, 255, 255) : RGB(255, 190, 80));
        TextOutA(dst, (int)g_browse.left + 8, y, sm->l1, (int)strlen(sm->l1));
        SetTextColor(dst, idx == g_sel ? RGB(220, 230, 255) : RGB(160, 180, 240));
        TextOutA(dst, (int)g_browse.left + 140, y, sm->l2, (int)strlen(sm->l2));
        char bars[16];
        snprintf(bars, sizeof(bars), "%d", sm->bars);
        SetTextColor(dst, RGB(120, 140, 200));
        TextOutA(dst, (int)g_browse.left + 250, y, bars, (int)strlen(bars));
        const char *file = strrchr(sm->path, '\\');
        if (file) TextOutA(dst, (int)g_browse.left + 276, y, file + 1, (int)strlen(file + 1));
    }

    /* The scroll thumb, sized and placed like the list it stands for. */
    if (gc > rows) {
        int track = (int)g_browse.bottom - (int)g_browse.top - 8;
        int th = track * rows / gc;
        if (th < 8) th = 8;
        int ty = (int)g_browse.top + 4 + (track - th) * g_scroll / (gc - rows);
        RECT thumb = { g_scroll_x, ty, g_scroll_x + 9, ty + th };
        HBRUSH hb = CreateSolidBrush(RGB(120, 150, 230));
        FillRect(dst, &thumb, hb);
        DeleteObject(hb);
    }

    SelectClipRgn(dst, NULL);
    DeleteObject(clip);
    SelectObject(dst, old);
    DeleteObject(fnt);

    /* Light the live category from the sheet. SEITEN calls these
     * OnOffRollOverButtons, so their third state is the "on" one - which is
     * exactly what a selected category is. */
    for (int i = 0; i < 12; i++) {
        char nm[16];
        snprintf(nm, sizeof(nm), "B_GRUPPE_%02d", i + 1);
        const KCTRL *c = find_ctrl(nm);
        /* Two states only - the third pair is 0,0 - so "on" is the second,
         * which is also the roll-over. eJay reuses it for both. */
        if (c) draw_ctrl(dst, c, i == g_group ? 1 : 0);
    }
}

/* ---- dragging a block along its lane -------------------------------------
 * The engine has no "move": APlay appends to a track and that is that. So a
 * drop rebuilds the arrangement - ACloseAll, place every slot again, AStart -
 * which is heavier than it sounds only if you are moving blocks constantly,
 * and is exactly what eJay itself has to do.
 *
 * ponytail: rebuild the whole arrangement on drop. Fine for sixty-four slots;
 * revisit if the engine turns out to have a per-entry edit. */
static int g_hover = -1, g_down = -1;

static void paint_ctrl(HWND wnd, int i, int state)
{
    if (i < 0 || i >= g_ctrl_n) return;
    HDC dc = GetDC(wnd);
    draw_ctrl(g_canvas, &g_ctrl[i], state);
    BitBlt(dc, g_ctrl[i].x, g_ctrl[i].y, g_ctrl[i].w, g_ctrl[i].h,
           g_canvas, g_ctrl[i].x, g_ctrl[i].y, SRCCOPY);
    ReleaseDC(wnd, dc);
}

static int g_astart = 0xA17FC0;   /* what AStart is told the song is */
static int g_drag = -1;      /* index into g_song, or -1 */
static int g_drag_dx;        /* grab offset within the block, in bars */

static int bar_width(void) { return (GRID_X1 - GRID_X0) / 16; }

static int slot_at(int x, int y)
{
    if (x < GRID_X0 || x >= GRID_X1 || y < GRID_Y0 || y >= GRID_Y1) return -1;
    int lane = (y - GRID_Y0) / 18;
    int bar  = (x - GRID_X0) / bar_width();
    for (int i = 0; i < g_song_n; i++)
        if (g_song[i].lane == lane && bar >= g_song[i].bar &&
            bar < g_song[i].bar + g_song[i].bars) return i;
    return -1;
}

static void rebuild_arrangement(void)
{
    static short st[64];
    fn_i_v ACloseAll = (fn_i_v) GetProcAddress(g_eng, "ACloseAll");
    fn_i_v AStop     = (fn_i_v) GetProcAddress(g_eng, "AStop");
    fn_i_i AStart    = (fn_i_i) GetProcAddress(g_eng, "AStart");
    /* AStop asks the mixer to stop, it does not wait for it. Freeing the track
     * entries out from under a pass that is still running is a crash, so give
     * the audio thread a moment to come out before ACloseAll takes them away.
     * ponytail: a fixed wait. A handshake would be better if one exists. */
    if (AStop) AStop();
    Sleep(40);
    if (ACloseAll) ACloseAll();
    for (int i = 0; i < g_song_n && g_aplay; i++) {
        const SLOT *sl = &g_song[i];
        if (sl->lib < 0 || sl->lib >= g_lib_n) continue;
        st[i] = 0x63;
        g_aplay(i + 1, 0, 0, 0, 0, &st[i], g_lib[sl->lib].path,
                sl->lane, sl->bar * BAR_UNITS, sl->bars * BAR_UNITS,
                0, 0x100);
    }
    if (AStart) AStart(g_astart);
}

static int browser_input(HWND h, UINT m, WPARAM w, LPARAM l)
{
    /* A block follows the pointer along its own lane, snapped to the bar grid,
     * and the arrangement goes back to the engine when it is dropped. */
    if (m == WM_MOUSEMOVE && g_drag >= 0) {
        SLOT *sl = &g_song[g_drag];
        int bar = ((short)LOWORD(l) - GRID_X0) / bar_width() - g_drag_dx;
        if (bar < 0) bar = 0;
        if (bar + sl->bars > 16) bar = 16 - sl->bars;
        if (bar != sl->bar) {
            sl->bar = bar;
            draw_blocks(g_canvas);
            HDC dc = GetDC(h);
            BitBlt(dc, GRID_X0, GRID_Y0, GRID_X1 - GRID_X0, GRID_Y1 - GRID_Y0,
                   g_canvas, GRID_X0, GRID_Y0, SRCCOPY);
            ReleaseDC(h, dc);
        }
        return 1;
    }
    if (m == WM_LBUTTONUP && g_drag >= 0) {
        const SLOT *sl = &g_song[g_drag];
        printf("  moved %s -> lane %d, bar %d\n",
               (sl->lib >= 0 && sl->lib < g_lib_n) ? g_lib[sl->lib].l1 : "?",
               sl->lane, sl->bar);
        rebuild_arrangement();
        g_drag = -1;
        ReleaseCapture();
        return 1;
    }
    if (m == WM_LBUTTONDOWN) {
        int hit = slot_at((short)LOWORD(l), (short)HIWORD(l));
        if (hit >= 0) {
            g_drag = hit;
            g_drag_dx = ((short)LOWORD(l) - GRID_X0) / bar_width() - g_song[hit].bar;
            SetCapture(h);
            return 1;
        }
    }

    /* Roll-over and press come straight off the sheet: only two controls are
     * ever lit at once, so this is two blits rather than a repaint. */
    if (m == WM_MOUSEMOVE || m == WM_LBUTTONUP) {
        int now = ctrl_at((short)LOWORD(l), (short)HIWORD(l));
        if (m == WM_LBUTTONUP && g_down >= 0) { paint_ctrl(h, g_down, 0); g_down = -1; }
        if (now != g_hover) {
            paint_ctrl(h, g_hover, 0);
            paint_ctrl(h, now, 1);
            g_hover = now;
        }
        if (m == WM_MOUSEMOVE) return 1;
    }
    if (m == WM_LBUTTONDOWN || m == WM_LBUTTONDBLCLK) {
        int hit = ctrl_at((short)LOWORD(l), (short)HIWORD(l));
        if (hit >= 0) { g_down = hit; paint_ctrl(h, hit, 2); }
    }

    /* The browser is the one part of this chrome that does something, so it
     * gets the input: a category button switches the sound group, the wheel or
     * a click in the scroll strip moves the list, a click on a row selects it,
     * and a double click places it. Which is what eJay's own tooltip says to
     * do - "double click for play back of a sample, move a sample to one of
     * the tracks". */
    if (m == WM_MOUSEWHEEL) {
        g_scroll -= GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA * 3;
        g_dirty = 1;
        return 1;
    }
    if (m == WM_LBUTTONDOWN || m == WM_LBUTTONDBLCLK) {
        int x = LOWORD(l), y = HIWORD(l);
        int grp = hit_group(x, y);
        if (grp >= 0) {
            if (grp < g_grp_n) { g_group = grp; g_scroll = 0; g_dirty = 1; }
            return 1;
        }
        if (x >= g_scroll_x && x < g_scroll_x + 9 &&
            y >= (int)g_browse.top && y < (int)g_browse.bottom) {
            int gc = g_grp_count[g_group], rows = browser_rows();
            if (gc > rows)
                g_scroll = (y - (int)g_browse.top) * (gc - rows) / ((int)g_browse.bottom - (int)g_browse.top);
            g_dirty = 1;
            return 1;
        }
        if (x >= (int)g_browse.left && x < (int)g_browse.right && y >= (int)g_browse.top && y < (int)g_browse.bottom) {
            int r = (y - (int)g_browse.top - BEZEL_T) / ROW_H;
            int idx = g_grp_start[g_group] + g_scroll + r;
            if (r >= 0 && r < browser_rows() &&
                g_scroll + r < g_grp_count[g_group] && idx < g_lib_n) {
                g_sel = idx;
                g_dirty = 1;
                if (m == WM_LBUTTONDBLCLK) g_place = idx;
            }
            return 1;
        }
    }
    return 0;
}

/* ---- did we paint outside the lines? --------------------------------------
 * Twice now the browser list has crept up over the panel frame, and both times
 * it was only caught by eye. Compare the window against the chrome it was built
 * from: every pixel that differs must be inside a region we are entitled to
 * paint - the arrangement grid, the browser panel, the category buttons. It
 * costs one blit and catches the whole class of mistake.
 */
static int paint_check(HWND wnd)
{
    if (!g_screen.hdc) return 0;
    RECT rc;
    GetClientRect(wnd, &rc);
    int w = rc.right, h = rc.bottom;
    int stride = ((w * 3) + 3) & ~3;
    unsigned char *a = (unsigned char *)malloc((size_t)stride * h);
    unsigned char *b = (unsigned char *)malloc((size_t)stride * h);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC wdc = GetDC(wnd), mdc = CreateCompatibleDC(wdc);
    HBITMAP bm = CreateCompatibleBitmap(wdc, w, h);
    HBITMAP old = (HBITMAP)SelectObject(mdc, bm);
    BitBlt(mdc, 0, 0, w, h, wdc, 0, 0, SRCCOPY);
    int ok = a && b &&
             GetDIBits(mdc, bm, 0, h, a, &bi, DIB_RGB_COLORS) &&
             GetDIBits((HDC)(INT_PTR)g_screen.hdc,
                       (HBITMAP)GetCurrentObject((HDC)(INT_PTR)g_screen.hdc, OBJ_BITMAP),
                       0, h, b, &bi, DIB_RGB_COLORS);
    long stray = 0;
    int fy = -1, fx = -1;
    for (int y = 0; ok && y < h; y++) {
        int sy = h - 1 - y;                     /* the DIBs come back bottom-up */
        for (int x = 0; x < w; x++) {
            const unsigned char *pa = a + (size_t)y * stride + x * 3;
            const unsigned char *pb = b + (size_t)y * stride + x * 3;
            int diff = abs(pa[0] - pb[0]) + abs(pa[1] - pb[1]) + abs(pa[2] - pb[2]);
            if (diff <= 30) continue;
            int inside =
                (sy >= GRID_Y0 && sy < GRID_Y1 && x >= GRID_X0 && x < GRID_X1) ||
                (sy >= (int)g_browse.top && sy < (int)g_browse.bottom &&
                 x >= (int)g_browse.left && x < (int)g_browse.right + g_scroll_w) ||
                (sy >= 340 && sy < 475 && ((x >= 100 && x < 170) || (x >= 555 && x < 635)));
            if (!inside) { if (fy < 0) { fy = sy; fx = x; } stray++; }
        }
    }
    free(a); free(b);
    SelectObject(mdc, old);
    DeleteObject(bm);
    DeleteDC(mdc);
    ReleaseDC(wnd, wdc);
    if (!ok) { printf("  paint check                -> could not read back\n"); return 0; }
    if (stray) printf("  paint check                -> %ld px outside, first at %d,%d\n",
                      stray, fx, fy);
    else printf("  paint check                -> ok, nothing outside the panels\n");
    return stray != 0;
}

/* The hit-testing and the scroll clamp are the only real logic in here, so
 * they get the one check: synthetic coordinates, no mouse, no window. Run with
 * --selftest. */
static int browser_selftest(void)
{
    int bad = 0;
    for (int i = 0; i < 12; i++) {
        int bx = (i < 6) ? BTN_LX : BTN_RX;
        int by = BTN_Y0 + (i % 6) * BTN_PITCH;
        int got = hit_group(bx + BTN_W / 2, by + BTN_H / 2);
        if (got != i) { printf("  ! button %d hit-tests as %d\n", i, got); bad++; }
    }
    if (hit_group((int)g_browse.left + 40, (int)g_browse.top + 40) != -1) {
        printf("  ! a click in the list hit-tests as a button\n"); bad++;
    }
    if (g_lib_n) {
        int save_g = g_group, save_s = g_scroll;
        g_group = 0;
        g_scroll = 1000000;                  /* far past the end */
        draw_browser(NULL);                  /* clamps as a side effect */
        if (g_scroll != g_grp_count[0] - browser_rows()) {
            printf("  ! scroll clamped to %d, expected %d\n",
                   g_scroll, g_grp_count[0] - browser_rows());
            bad++;
        }
        g_scroll = -5;
        draw_browser(NULL);
        if (g_scroll != 0) { printf("  ! negative scroll not clamped\n"); bad++; }
        g_group = save_g; g_scroll = save_s;
    }
    printf("  browser selftest           -> %s\n", bad ? "FAILED" : "ok");
    return bad;
}

static void draw_cursor(HWND wnd, double frac)
{
    static int last = -1;
    if (!g_screen.hdc) return;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    g_cursor_frac = frac;
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

/* A crash in a DLL nobody has source for is only useful if it says where. Name
 * the module and the offset into it, which is enough to find the function in
 * the same disassembly everything else here came out of. */
static LONG WINAPI report_fault(EXCEPTION_POINTERS *ep)
{
    void *pc = (void *)ep->ExceptionRecord->ExceptionAddress;
    HMODULE mod = NULL;
    char name[MAX_PATH] = "?";
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)pc, &mod);
    if (mod) GetModuleFileNameA(mod, name, sizeof(name));
    const char *tail = strrchr(name, '\\');
    printf("\n!! crash: exception %08lX at %p\n",
           (unsigned long)ep->ExceptionRecord->ExceptionCode, pc);
    printf("   in %s + %08lX\n", tail ? tail + 1 : name,
           mod ? (unsigned long)((char *)pc - (char *)mod) : 0ul);
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        printf("   %s address %p\n",
               ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
               (void *)ep->ExceptionRecord->ExceptionInformation[1]);
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

/* An unhandled-exception filter never runs if the DLL has a __try that
 * swallows the fault first, and PXD32CL1 has several. A vectored handler sees
 * every exception first-chance, before any of that. */
static LONG CALLBACK first_chance(EXCEPTION_POINTERS *ep)
{
    static int shown;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    /* Every code, not just access violations: a stack overflow leaves no stack
     * to print from later, so it has to be caught here or not at all. C++
     * exceptions and the thread-naming marker are noise. */
    if (code != 0xE06D7363 && code != 0x406D1388 && shown < 8) {
        void *pc = (void *)ep->ExceptionRecord->ExceptionAddress;
        HMODULE mod = NULL;
        char name[MAX_PATH] = "?";
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)pc, &mod);
        if (mod) GetModuleFileNameA(mod, name, sizeof(name));
        const char *tail = strrchr(name, '\\');
        printf("  !! exception %d: code %08lX  %s + %08lX\n", ++shown,
               (unsigned long)code, tail ? tail + 1 : name,
               mod ? (unsigned long)((char *)pc - (char *)mod) : 0ul);
        if (code == EXCEPTION_ACCESS_VIOLATION)
            printf("     %s address %p\n",
                   ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                   (void *)ep->ExceptionRecord->ExceptionInformation[1]);
        fflush(stdout);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Run one Refresh on a thread of our choosing, so the stack size is ours. */
static fn_i_i g_probe_refresh;
static int    g_probe_ms, g_probe_rc;

static DWORD WINAPI probe_thread(LPVOID unused)
{
    (void)unused;
    g_probe_rc = g_probe_refresh(g_probe_ms);
    return 0;
}

int main(int argc, char **argv)
{
    AddVectoredExceptionHandler(1, first_chance);
    SetUnhandledExceptionFilter(report_fault);
    setvbuf(stdout, NULL, _IONBF, 0);   /* a crash must not eat the trail */
    int ticks = 120, volume = 20, atyp = 3, dplay = 0, chan = 9;
    int intro = 1, scrcap = 0, frames = 0, verbose = 0, main_screen = 0, samples = 0;
    int seq = 0, trace_files = 0, song = 0, selftest = 0;
    int probe_ms = -1, probe_key = 0, dragtest = 0, first_minus1 = 0, ending = 0, flat = 0, limit = 0, watch = 0, bigstack = 0, paintcheck = 0, playpos = 0, playlen = 0, astart = 0xA17FC0, ids = 0, trackdump = 0;
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
        else if (!strcmp(argv[i], "--selftest")) selftest = 1;
        else if (!strcmp(argv[i], "--refresh") && i + 1 < argc) probe_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--probekey") && i + 1 < argc) probe_key = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dragtest")) dragtest = 1;
        else if (!strcmp(argv[i], "--watch")) watch = 1;
        else if (!strcmp(argv[i], "--paintcheck")) paintcheck = 1;
        else if (!strcmp(argv[i], "--pos") && i + 1 < argc) playpos = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ids")) ids = 1;
        else if (!strcmp(argv[i], "--tracks")) trackdump = 1;
        else if (!strcmp(argv[i], "--astart") && i + 1 < argc)
            g_astart = astart = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bigstack")) bigstack = 1;
        else if (!strcmp(argv[i], "--flat") && i + 1 < argc) flat = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--limit") && i + 1 < argc) limit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--file") && i + 1 < argc) { playfile = argv[++i]; seq = 1; }
        else if (!strcmp(argv[i], "--len") && i + 1 < argc) playlen = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--init1")) first_minus1 = 1;
        else if (!strcmp(argv[i], "--ending")) ending = 1;
        else if (!strcmp(argv[i], "--group") && i + 1 < argc) g_group = atoi(argv[++i]);
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

    if (libdir) {
        printf("  sample library             -> %d samples under %s\n",
               load_index(libdir), libdir);
        build_song();
        printf("  arrangement                -> %d slots\n", g_song_n);
    }
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

    if (intro && g_gfx) {
        snap_take(g_gfx);
        patch_import(g_gfx, "GDI32.dll", "BitBlt",
                     (void *)count_bitblt, (void **)&g_real_bitblt);
        patch_import(g_gfx, "GDI32.dll", "PatBlt",
                     (void *)count_patblt, (void **)&g_real_patblt);
        patch_import(g_gfx, "GDI32.dll", "SetPixel",
                     (void *)count_setpixel, (void **)&g_real_setpixel);
    }

    if (intro && ALoad && InitScreen) {
        ALOADREC scr, leds, text;
        char path[MAX_PATH];

        /* Register the intro's own elements. SEITEN lists 58 of them under
         * `:Intro` - the LED field, three progress bars and their percentages,
         * three lines of text and twenty-four VU segments - and K_640 gives
         * each a name and ten numbers. GFX_IntroSetKey takes exactly that: a
         * name and ten values. Without them the DLL reaches the point where it
         * would draw those VU segments with nothing to draw them from, which is
         * where the animation was stopping. */
        fn_setkey SetKey = (fn_setkey) GetProcAddress(g_gfx, "GFX_IntroSetKey");
        int keys = 0;
        if (SetKey && load_keys("K_640")) {
            for (int i = 0; i < g_ctrl_n; i++) {
                const KCTRL *c = &g_ctrl[i];
                if (strncmp(c->name, "K_INTRO", 7)) continue;
                const int *n = c->raw;
                /* The first four arguments are x, y, w, h - the DLL stores
                 * left=a1, top=a2, right=a1+a3, bottom=a2+a4, which a memory
                 * diff across one call says plainly. In a K_640 record the size
                 * is the LAST pair, not the second, so passing the ten numbers
                 * in file order hands it the source coordinates as a width. */
                SetKey(c->name, n[0] / KSCALE, n[1] / KSCALE,
                       n[8] / KSCALE, n[9] / KSCALE,
                       n[2] / KSCALE, n[3] / KSCALE, n[4] / KSCALE,
                       n[5] / KSCALE, n[6] / KSCALE, n[7] / KSCALE);
                keys++;
            }
        }
        printf("  GFX_IntroSetKey x%d\n", keys);
        if (watch) snap_diff(g_gfx, "SetKey x58", 0x2EAC8 + 0x0,
                             0x2EAC8 + 0x8500, 200);

        /* Off by default: it grabs the whole desktop and does not come back
         * promptly on a modern display. */
        if (scrcap && Capture) printf("  GFX_IntroDoScrCapture      -> %d\n", Capture());
        if (watch) snap_diff(g_gfx, "DoScrCapture", 0x2EAC8, 0x2EAC8 + 0x8500, 200);

        snprintf(path, sizeof(path), "%s\\EJAY31A", gfxdir);
        if (load_bitmap(ALoad, &scr, path)) {
            printf("  GFX_IntroInitScreen        -> %d\n",
                   InitScreen(scr.hdc, scr.w, scr.h, scr.bits, scr.pal));
            if (watch) snap_diff(g_gfx, "InitScreen", 0x2EAC8, 0x2EAC8 + 0x8500, 200);
            /* The copy surface has to be real. GFX_IntroInitScreenCopy sets a
             * rect from w,h and then tests the pixel pointer - and bails if it
             * is null, before it stores the device context at +0x79C that the
             * animation later blits from. Passing null there is a crash waiting
             * at t > 3400ms: reading [NULL+4] for a BitBlt source.
             *
             * So load the screen bitmap a second time and hand that over. A
             * scratch copy of the screen is what the name says it wants, and an
             * independent DIB gives the fade something to fade from. */
            if (InitCopy) {
                /* And it has to be 16-bit. The blend loop inside the DLL ends
                 * `mov word ptr [edi], ax`, packing 5-6-5 out of a 24-bit
                 * colour, so it writes two bytes a pixel. Handing it one of the
                 * 8bpp GRAFIKA bitmaps gives it half the buffer it believes it
                 * has; it runs off the end, catches that in its own __try and
                 * carries on - drawing correctly while corrupting whatever
                 * follows the DIB. A scratch 5-6-5 surface of the right size is
                 * what it actually wants. */
                HDC wdc = GetDC(wnd);
                struct { BITMAPINFOHEADER h; DWORD mask[3]; } bi;
                memset(&bi, 0, sizeof(bi));
                bi.h.biSize = sizeof(BITMAPINFOHEADER);
                bi.h.biWidth = scr.w;
                bi.h.biHeight = scr.h;
                bi.h.biPlanes = 1;
                bi.h.biBitCount = 16;
                bi.h.biCompression = BI_BITFIELDS;
                bi.mask[0] = 0xF800; bi.mask[1] = 0x07E0; bi.mask[2] = 0x001F;
                void *cbits = NULL;
                HBITMAP cbm = CreateDIBSection(wdc, (BITMAPINFO *)&bi,
                                               DIB_RGB_COLORS, &cbits, NULL, 0);
                HDC cdc = CreateCompatibleDC(wdc);
                if (cbm && cdc) SelectObject(cdc, cbm);
                ReleaseDC(wnd, wdc);
                printf("  copy surface               -> %dx%d 16bpp, bits %p\n",
                       scr.w, scr.h, cbits);
                printf("  GFX_IntroInitScreenCopy    -> %d\n",
                       InitCopy((int)(INT_PTR)cdc, scr.w, scr.h, cbits));
            }
            if (watch) snap_diff(g_gfx, "InitScreenCopy", 0x2EAC8, 0x2EAC8 + 0x8500, 200);
        } else {
            printf("  ! no bitmap - run this from the ejay folder\n");
        }

        snprintf(path, sizeof(path), "%s\\EJAY33A", gfxdir);
        if (InitLeds && load_bitmap(ALoad, &leds, path))
            printf("  GFX_IntroInitLeds          -> %d\n",
                   InitLeds(leds.hdc, leds.w, leds.h, leds.bits));
            if (watch) snap_diff(g_gfx, "InitLeds", 0x2EAC8, 0x2EAC8 + 0x8500, 200);

        snprintf(path, sizeof(path), "%s\\EJAY32A", gfxdir);
        if (InitText && load_bitmap(ALoad, &text, path))
            printf("  GFX_IntroInitText          -> %d\n",
                   InitText(text.hdc, text.w, text.h, text.bits));
            if (watch) snap_diff(g_gfx, "InitText", 0x2EAC8, 0x2EAC8 + 0x8500, 200);

        /* -1 is not an init, whatever it looked like. Its dispatch compares
         * the argument before it looks at the "started" byte, and the branch it
         * reaches sets that byte and switches every later call to a different
         * renderer - the ending, which is why Dancejay calls it in the loop
         * before GFX_IntroClose and nowhere else. Driving it first makes the
         * animation never run at all, which is what "no stall, black screen"
         * was. Off unless asked for. */
        if (Refresh && ending)
            printf("  GFX_IntroRefresh(-1)       -> %d\n", Refresh(-1));

        /* Distinctive values into one element, then read the rect back: the
         * only way to learn which of the ten arguments become which corner. */
        if (SetKey && probe_key) {
            /* Guessing the object layout got nowhere, so ask the memory
             * instead: feed in ten values that cannot occur naturally, then
             * sweep the DLL's data for them. Wherever they land is where the
             * rect goes, and which ones land tells us the argument order. */
            /* The arguments are combined before they are stored, so matching
             * markers only finds the ones that pass through untouched. Diff the
             * whole image across the call instead: every dword that moves is
             * somewhere SetKey writes, whatever arithmetic it did first. */
            const char *gb = (const char *)g_gfx;
            IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)gb;
            IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(gb + dos->e_lfanew);
            DWORD span = nt->OptionalHeader.SizeOfImage;
            int *before = (int *)malloc(span);
            memcpy(before, gb, span);
            /* Two different names: if the slot is chosen by name, two
             * different addresses move. If the same one moves twice, the name
             * is not selecting anything and the elements must be created by
             * something else first. */
            enum { MARK = 31000 };
            SetKey(probe_key == 2 ? "K_INTRO_VU02" : "K_INTRO_VU01",
                   MARK + 1, MARK + 2, MARK + 3, MARK + 4,
                   MARK + 5, MARK + 6, MARK + 7, MARK + 8, MARK + 9, MARK + 10);
            int shown = 0;
            for (DWORD off = 0; off + 4 <= span && shown < 40; off += 4) {
                int a0 = before[off / 4], a1 = *(const int *)(gb + off);
                if (a0 == a1) continue;
                printf("    +%06lX  %d -> %d", (unsigned long)off, a0, a1);
                if (a1 > MARK && a1 <= MARK + 10) printf("   = arg%d", a1 - MARK);
                putchar('\n');
                shown++;
            }
            if (!shown) printf("    SetKey wrote nothing at all\n");
            free(before);
        }
        /* Read the rects back out of the DLL. The intro object is a static at
         * base+0x2EAC8; its twenty-four VU elements live at +0x7990, 0x60
         * apart, each holding left/top/right/bottom at +0x34..+0x40. An element
         * the DLL never got coordinates for keeps whatever was in that memory,
         * and the fill routine then runs over it - which is the stall. */
        if (probe_key) {
            const char *gb = (const char *)g_gfx;
            for (int i = 0; i < 4; i++) {
                const int *r = (const int *)(gb + 0x2EAC8 + 0x7990 + i * 0x60 + 0x34);
                printf("    VU%02d rect %d,%d .. %d,%d\n", i + 1,
                       r[0], r[1], r[2], r[3]);
            }
        }
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
        printf("  K_640 controls             -> %d\n", load_keys("K_640"));
        browser_geometry();
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

        /* Keep the handles: the grid is redrawn whenever a sample is added. */
        g_zeich = Zeich; g_griddc = griddc;
        g_tex[0] = tex[0]; g_tex[1] = tex[1]; g_tex[2] = tex[2];
        draw_blocks(g_canvas);
        printf("  blocks drawn into the grid -> %d\n", g_song_n);

        draw_browser(g_canvas);
        printf("  browser                    -> %d groups, %d samples\n",
               g_grp_n, g_lib_n);
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
     * and its play button then does ASetFader(0,0) -> AStart(astart) and
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
    g_aplay = APlay;
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
            for (int i = 0; i < g_song_n && i < 64 && (!limit || i < limit); i++) {
                const SLOT *sl = &g_song[i];
                if (sl->lib < 0 || sl->lib >= g_lib_n) continue;
                st[i] = 0x63;
                /* flat==1 pins every sample to track 0 at position 0, which
                 * is the shape that plays at full scale when a single sample is
                 * placed. If the arrangement is quiet and this is not, the
                 * fault is in the track or the position, not the samples. */
                /* Argument 1 is a sample id, stored as a word at entry+0x10.
                 * Dancejay's real placement sites pass a distinct one per
                 * sample; every simple site passes 0. Thirteen entries all
                 * claiming id 0 is a plausible reason for thirteen samples
                 * sounding like one. */
                /* Argument 1 is a sample id, stored as a word at entry+0x10.
                 * Dancejay's real placement sites pass a distinct one per
                 * sample and every simple site passes 0; thirteen entries all
                 * claiming id 0 is why thirteen samples sounded like one.
                 *
                 * Argument 10 is the length, and it matters more than it looks:
                 * APlay stores entry+4 = start + length, but only when length
                 * is positive - pass zero and it writes 0x6921CFF0 instead, a
                 * sentinel that means "no end". An entry with no end plays from
                 * the moment the transport starts, which is why every block
                 * fired at once however far along the grid it was drawn. */
                int start = sl->bar * BAR_UNITS;
                /* The sample's own declared length, not a multiple of the bar:
                 * the engine stores entry+4 = start + len and stops there, so a
                 * length short of the audio cut every block off part-way. */
                int len   = sl->bars * BAR_UNITS;
                APlay(ids ? i + 1 : 0, 0, 0, 0, 0, &st[i], g_lib[sl->lib].path,
                      flat ? 0 : sl->lane,
                      flat ? 0 : start, flat ? 0 : len, 0, 0x100);
                placed++;
            }
            printf("  APlay x%d across %d tracks\n", placed, g_song_n);
        } else if (APlay) {
            /* One sample, placed where --pos says. If argument 9 is samples at
             * 44,100 then --pos 176400 should come in at four seconds and not
             * before; if it is anything else, it will not come in at all. That
             * is the whole question about the arrangement, in one variable. */
            int r = APlay(1, 0, 0, 0, 0, &status, full, 0, playpos,
                          playlen ? playlen : BAR_UNITS, 0, 0x100);
            printf("  APlay(%s) at %d -> %d, status %d\n", full, playpos, r, status);
        }
        peek("after APlay");
        if (trackdump) tracks("after APlay", 8);
        /* The intro function's own order: AFenster, then DStart, then AStart -
         * DStart is not only the sample-preview path's business. */
        {
            fn_i_i DStart = (fn_i_i) GetProcAddress(g_eng, "DStart");
            if (AFenster) AFenster((int)(INT_PTR)wnd);
            if (DStart)   printf("  DStart(0)                  -> %d\n", DStart(0));
        }
        if (ASetFader)  ASetFader(0, 0);
        if (AStart)     printf("  AStart(astart)           -> %d\n", AStart(astart));
        peek("after AStart");
        if (trackdump) tracks("after AStart", 8);
    }

    if (selftest && browser_selftest()) return 2;
    /* One Refresh at a chosen timestamp, timed. The animation has eight
     * thresholds in it and only one of the bands is slow, so the way to find
     * which is to ask rather than to read. */
    /* Neither handler fires and the process still dies, which is what a stack
     * overflow looks like - the handler would have to run on the stack that has
     * just been exhausted. So try the call on a thread with a big one. If that
     * is the difference, the DLL simply wants more stack than a default thread
     * has, and saying so is the fix. */
    if (probe_ms >= 0 && Refresh && bigstack) {
        g_probe_refresh = Refresh;
        g_probe_ms = probe_ms;
        DWORD t = GetTickCount();
        HANDLE th = CreateThread(NULL, 64u << 20, probe_thread, NULL, 0, NULL);
        DWORD w = th ? WaitForSingleObject(th, 120000) : WAIT_FAILED;
        printf("  Refresh(%d) on a 64MB stack -> %d in %lu ms (wait %lu)\n",
               probe_ms, g_probe_rc, (unsigned long)(GetTickCount() - t),
               (unsigned long)w);
        if (th) CloseHandle(th);
        return 0;
    }

    if (probe_ms >= 0 && Refresh) {
        patch_import(g_gfx, "GDI32.dll", "BitBlt",
                     (void *)count_bitblt, (void **)&g_real_bitblt);
        patch_import(g_gfx, "GDI32.dll", "PatBlt",
                     (void *)count_patblt, (void **)&g_real_patblt);
        patch_import(g_gfx, "GDI32.dll", "SetPixel",
                     (void *)count_setpixel, (void **)&g_real_setpixel);
        /* Dump one element before the call. The routine that eats the time is
         * a software pixel loop, not GDI - 24 BitBlts in 83 seconds - and its
         * iteration count comes out of these fields. */
        {
            const char *e = (const char *)g_gfx + 0x2EAC8 + 0x7990;
            for (int f = 0; f < 0x60; f += 4) {
                int v = *(const int *)(e + f);
                float fv = *(const float *)(e + f);
                if (v) printf("    element0 +%02X  %11d  %g\n", f, v, fv);
            }
        }
        /* -1 is the first-time init: with the "started" byte at +0x368 still
         * clear, that is the only argument that reaches the branch which sets
         * it, and every other value walks straight into the animation with the
         * elements never having been prepared. */
        if (first_minus1) printf("    Refresh(-1) -> %d\n", Refresh(-1));
        DWORD t = GetTickCount();
        int rc = Refresh(probe_ms);
        printf("  Refresh(%d) -> %d in %lu ms\n", probe_ms, rc,
               (unsigned long)(GetTickCount() - t));
        printf("    BitBlt %ld, PatBlt %ld, SetPixel %ld\n",
               g_n_bitblt, g_n_patblt, g_n_setpixel);
        return 0;
    }
    float peak = 0.0f;
    static float env[2400];
    static int   envt[2400];   /* AGetTime beside each level, so the time axis
                                 is the engine's playback clock and not an
                                 assumption about how long a tick took */
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
    /* --ticks 0 keeps the window up until it is closed, which is the only way
     * to actually click anything. */
    for (int i = 0; ticks == 0 || i < ticks; i++) {
        if (ticks == 0 && !IsWindow(wnd)) break;
        /* Synthesise the drag gesture so it can be checked without a mouse:
         * grab the first block, walk it six bars along its lane, drop it. */
        if (dragtest && i == 20 && g_song_n) {
            int bw = bar_width();
            int y = GRID_Y0 + g_song[0].lane * 18 + 8;
            int x0 = GRID_X0 + g_song[0].bar * bw + 4;
            printf("  dragtest: block 0 at bar %d\n", g_song[0].bar);
            browser_input(wnd, WM_LBUTTONDOWN, 0, MAKELPARAM(x0, y));
            for (int b = 1; b <= 6; b++)
                browser_input(wnd, WM_MOUSEMOVE, 0,
                              MAKELPARAM(x0 + b * bw, y));
            browser_input(wnd, WM_LBUTTONUP, 0, MAKELPARAM(x0 + 6 * bw, y));
            printf("  dragtest: now at bar %d (expected %d)\n",
                   g_song[0].bar, 6);

            /* And the gesture that crashed: press on a browser row, drag up
             * into the track grid, release there. Nothing places a sample that
             * way yet, but it must not fall over either. */
            int by = (int)g_browse.top + BEZEL_T + 2 * ROW_H + 4;
            int bx = (int)g_browse.left + 40;
            browser_input(wnd, WM_LBUTTONDOWN, 0, MAKELPARAM(bx, by));
            for (int k = 1; k <= 8; k++)
                browser_input(wnd, WM_MOUSEMOVE, 0,
                              MAKELPARAM(bx + k * 20, by - k * 40));
            browser_input(wnd, WM_LBUTTONUP, 0,
                          MAKELPARAM(GRID_X0 + 200, GRID_Y0 + 40));
            printf("  dragtest: browser-to-grid drag survived\n");
        }
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
            /* AGetTime reports milliseconds - it divides the engine's byte
             * position by 882 and multiplies by 5 - while AStart's 0xA17FC0 is
             * samples. Sixteen bars at 120 BPM is 32,000 ms, and that is what
             * the grid is showing. */
            /* Sixteen bars of grid, and a bar is BAR_UNITS output bytes at
                 * 176.4 per millisecond - 27.4 seconds, not the 32 guessed here
                 * when a bar was thought to be 120 BPM. */
                draw_cursor(wnd, AGetTime(0) / (BAR_UNITS / 176.4 * 16.0));
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

        /* A double click in the browser puts that sample into the first lane
         * with room at the bar the cursor is on, exactly the gesture eJay's own
         * tooltip describes. The engine takes it live: APlay appends to the
         * track record, and the same slot is drawn into the grid, so the
         * picture and the arrangement stay the same list. */
        if (g_place >= 0 && g_place < g_lib_n) {
            if (g_song_n < (int)(sizeof(g_song) / sizeof(g_song[0]))) {
                int atbar = (int)(16.0 * g_cursor_frac);
                int lane = 0;
                for (; lane < 16; lane++) {
                    int clash = 0;
                    for (int k = 0; k < g_song_n; k++)
                        if (g_song[k].lane == lane &&
                            atbar < g_song[k].bar + g_song[k].bars &&
                            g_song[k].bar < atbar + g_lib[g_place].bars) clash = 1;
                    if (!clash) break;
                }
                if (lane < 16) {
                    SLOT *sl = &g_song[g_song_n++];
                    sl->lane = lane; sl->bar = atbar;
                    sl->bars = g_lib[g_place].bars; sl->lib = g_place;
                    if (g_aplay) {
                        static short pst;
                        pst = 0x63;
                        g_aplay(g_song_n, 0, 0, 0, 0, &pst, g_lib[g_place].path,
                                lane, atbar * BAR_UNITS,
                                sl->bars * BAR_UNITS, 0, 0x100);
                    }
                    printf("  placed %s / %s -> lane %d, bar %d, %d bars\n",
                           g_lib[g_place].l1, g_lib[g_place].l2, lane, atbar,
                           sl->bars);
                    draw_blocks(g_canvas);
                    {
                        HDC dc = GetDC(wnd);
                        BitBlt(dc, GRID_X0, GRID_Y0, GRID_X1 - GRID_X0,
                               GRID_Y1 - GRID_Y0, g_canvas, GRID_X0, GRID_Y0, SRCCOPY);
                        ReleaseDC(wnd, dc);
                    }
                }
            }
            g_place = -1;
            g_dirty = 0;
        }
        if (g_dirty) {
            HDC dc = GetDC(wnd);
            draw_browser(g_canvas);
            BitBlt(dc, (int)g_browse.left - 60, (int)g_browse.top - 24,
                   640 - ((int)g_browse.left - 60), (int)g_browse.bottom - (int)g_browse.top + 24,
                   g_canvas, (int)g_browse.left - 60, (int)g_browse.top - 24, SRCCOPY);
            ReleaseDC(wnd, dc);
            g_dirty = 0;
        }
        /* RTimer and the per-frame drawing together cost about as long as the
         * sleep does, so pumping once a frame leaves the window feeling stuck
         * under the mouse. Pump again now the work is done. */
        pump_messages();
        float p = meter_peak();
        if (p > peak) peak = p;
        if (env_n < (int)(sizeof(env) / sizeof(env[0]))) {
            envt[env_n] = AGetTime ? AGetTime(0) : 0;
            env[env_n++] = p;
        }
        if (seq && i % 50 == 0)
            printf("    t=%5lums  AGetTime %8d  transport %9d  cur %d/%d  peak %.3f\n",
                   (unsigned long)(GetTickCount() - t0),
                   AGetTime ? AGetTime(0) : -1,
                   *(int *)((char *)g_eng + 0x3A0C0),
                   *(short *)((char *)g_eng + 0x429C8 + 0x68),
                   *(short *)((char *)g_eng + 0x429C8 + 0x6A), peak);
        /* The engine says when it fired an entry: the track's cursor steps on
         * as the transport passes each start. Printing AGetTime - real playback
         * milliseconds - at that moment measures the unit of APlay's start
         * argument directly, instead of inferring it from a meter that lags and
         * a sample that fades in. */
        if (trackdump) {
            static short seen[8];
            for (int t = 0; t < 8; t++) {
                const char *tr = (const char *)g_eng + 0x429C8 + t * 0x84;
                short cur = *(const short *)(tr + 0x68);
                if (cur == seen[t]) continue;
                seen[t] = cur;
                const char *ents = *(const char *const *)(tr + 0x64);
                if (!ents || cur < 1) continue;
                printf("    fired track %d entry %d start %d at AGetTime %d ms  transport %d\n",
                       t, cur - 1, *(const int *)(ents + (cur - 1) * 0x108),
                       AGetTime ? AGetTime(0) : -1,
                       *(const int *)((const char *)g_eng + 0x3A0C0));
            }
        }
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
    if (g_n_bitblt || g_n_patblt || g_n_setpixel)
        printf("  graphics DLL did BitBlt %ld, PatBlt %ld, SetPixel %ld\n",
               g_n_bitblt, g_n_patblt, g_n_setpixel);
    /* Keep the envelope, not just the maximum. Static sits flat near full
     * scale; a loop has a beat in it, and the difference is visible in one
     * column of asterisks without anyone having to listen to it first. */
    if (env_n > 8) {
        /* The measured envelope beside the arrangement that was asked for: each
         * row is 128ms, `want` counts the blocks that should be sounding then.
         * A row with blocks and no level is a sample that did not play; a row
         * with level and no blocks is one that overran. That comparison is what
         * says the timing is right, without anyone having to listen first. */
        printf("  envelope vs arrangement, one row per 8 ticks (128ms):\n");
        printf("     bar  level  want\n");
        for (int k = 0; k + 8 <= env_n; k += 8) {
            float m = 0;
            for (int j = 0; j < 8; j++) if (env[k + j] > m) m = env[k + j];
            double at = envt[k] * 176.4;           /* playback ms -> output bytes */
            int want = 0;
            for (int i = 0; i < g_song_n; i++) {
                const SLOT *sl = &g_song[i];
                if (sl->lib < 0 || sl->lib >= g_lib_n) continue;
                double b0 = (double)sl->bar * BAR_UNITS;
                /* The sample's own length, not the bars its block occupies: a
                 * one-beat hit fills a bar of grid but only sounds for a beat,
                 * and counting it as a bar would report silence as a fault. */
                double b1 = b0 + (g_lib[sl->lib].len ? g_lib[sl->lib].len * 2.0
                                                     : sl->bars * (double)BAR_UNITS);
                if (at >= b0 && at < b1) want++;
            }
            printf("    %4.1f  %5.3f  %-4d |", at / BAR_UNITS, m, want);
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

    if (paintcheck) paint_check(wnd);
    capture(wnd, shot);
    printf("  done\n");
    FreeLibrary(g_eng);
    if (g_gfx) FreeLibrary(g_gfx);
    return 0;
}
