/* ejay2.c - drive an eJay 2-generation title's own engine and graphics DLLs.
 *
 * Dance eJay 2 (1999) and HipHop eJay 2 (2000) are a Visual Basic front end
 * over two DLLs: PXD32?4.DLL, the audio engine (D4 for Dance, H4 for HipHop),
 * and PXD32CL1.DLL, which draws. This host is our front end over the same two.
 * On Windows they load and run as themselves; on Linux and macOS they are
 * lifted to C and run on pcrecomp's win32hle (plat.h says how one source does
 * both).
 *
 * The call sequence is not a guess. Dancejay.exe is VB5 native, which does not
 * put `Declare Function` targets in the import table - it emits a lazy thunk
 * per API holding the DLL and export name. tools/vb_declares.py recovers that
 * mapping and disassembles every call site, so the order and the arguments
 * below are read out of the shipping program:
 *
 *     ATyp(3) -> ADevice(0) -> AInit(hwnd) -> GFX_AnimationSwapInit(400, 400)
 *
 * ATyp first is the part that matters: it selects the engine profile, and
 * ADevice enumerates against it. docs/EJAY2.md has the rest.
 *
 * Volume is pinned low on purpose: a bring-up run queues whatever the mixer
 * happens to hold.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include "plat.h"

/* The engine is named for its title and the graphics DLL is the same name in
 * every title, so --engine is all it takes to point this host at a sibling.
 * PXD32H4 exports every one of PXD32D4's 87 functions and adds seven. */
static const char *g_engine_name = "PXD32D4.DLL";
static const char *g_gfx_name    = "PXD32CL1.DLL";

/* ALoad's argument, recovered from its own code: it OpenFile()s the name at
 * +0x14, parses the BMP (LZ-decompressing it first if the magic is SZDD, which
 * three of the GRAFIKA files are), CreateDIBSection()s it and hands back the DC
 * and the pixels. So the graphics DLL never sees a file - it is given a live
 * DIB, and eJay's whole art pipeline is this one call. The name is a VB
 * fixed-length string, stored inline, 220 chars. */
typedef struct {
    int32_t hdc;    /* memory DC with the DIB selected */
    int32_t w, h;   /* biWidth, biHeight */
    gva     bits;   /* DIB section pixels */
    gva     pal;    /* the BITMAPINFO colour table, 256 RGBQUADs */
    char    name[220];
} ALOADREC;

static gva g_eng, g_gfx;
/* The workspace chrome and the sheet its controls are cut from, kept so the
 * window can be repainted from them. In guest memory: ALoad writes them. */
static ALOADREC *g_screen, *g_sheet;
/* Chrome + sample blocks, composited once. The cursor erases back to this
 * rather than to the bare chrome, or it would wipe the arrangement. */
static HDC g_canvas;
/* set by the window procedure, acted on by the main loop */
static int g_dirty = 1;
static int g_place = -1;
/* the graphics DLL handles the grid is drawn with, and the engine call that
 * places a sample - both needed again whenever the arrangement changes */
static gva g_zeich, g_aplay, g_dplayupd;
static int g_griddc;
static int g_tex[3];
static double g_cursor_frac;

static gva need(gva mod, const char *name)
{
    gva p = mod ? dll_proc(mod, name) : 0;
    if (!p) printf("  ! %s not found\n", name);
    return p;
}
static gva eng(const char *name) { return g_eng ? dll_proc(g_eng, name) : 0; }
static gva gfx(const char *name) { return g_gfx ? dll_proc(g_gfx, name) : 0; }

/* The engine's own state, read in place. */
#define AT(va, T) (*(volatile T *)HOSTP(va))

/* Two of the engine's addresses the host needs, found in its own code rather
 * than hardcoded, because every title's engine is a separate build:
 *
 *   g_tracks  the track records, 0x84 apart. APlay finds track n as
 *             `imul eax, eax, 0x84; add eax, <tracks>`, then reads the
 *             entry count at +0x6A. Count +0x6A, cursor +0x68, entry array +0x64.
 *   g_armed   the word the mixer will not run without. RTimer's first call is
 *             the mixer tick, whose first `movsx r32, word ptr [armed];
 *             test r32, r32; jle` tests it (ecx in PXD32D4, H4 and R4; eax
 *             in PXD32D5).
 *
 * A pattern that is not there leaves 0, and the host does without (see the
 * main loop). g_d4 marks Dance eJay 2's own engine, the one whose other
 * offsets --verbose and --tracks print. */
static gva g_tracks, g_armed;
static int g_d4;

static gva find_after(gva at, int span, const char *pat, int patlen, int off)
{
    const uint8_t *b = (const uint8_t *)HOSTP(at);
    for (int i = 0; b && i + patlen + off + 4 <= span; i++)
        if (!memcmp(b + i, pat, (size_t)patlen)) {
            uint32_t v;
            memcpy(&v, b + i + off, 4);
            return v;
        }
    return 0;
}

static void find_layout(void)
{
    gva aplay = eng("APlay"), rtimer = eng("RTimer");
    if (aplay) {
        gva t = find_after(aplay, 0x200, "\x69\xC0\x84\x00\x00\x00\x05", 7, 7);
        if (t && find_after(aplay, 0x200, "\x0F\xBF\x51\x6A", 4, 0)) g_tracks = t;
    }
    if (rtimer) {
        const uint8_t *r = (const uint8_t *)HOSTP(rtimer);
        for (int i = 0; i < 0x40; i++)
            if (r[i] == 0xE8) {                        /* call rel32: the mixer tick */
                int32_t rel;
                memcpy(&rel, r + i + 1, 4);
                gva tick = rtimer + (uint32_t)i + 5 + (uint32_t)rel;
                const uint8_t *b = (const uint8_t *)HOSTP(tick);
                for (int k = 0; k + 10 < 0x200; k++)
                    if (b[k] == 0x0F && b[k + 1] == 0xBF && (b[k + 2] & 0xC7) == 0x05 &&     /* movsx r, [abs32] */
                        b[k + 7] == 0x85 && b[k + 8] == (0xC0 | (b[k + 2] & 0x38) | ((b[k + 2] >> 3) & 7)) &&
                        b[k + 9] == 0x7E) {
                        memcpy(&g_armed, b + k + 3, 4);
                        break;
                    }
                break;
            }
    }
    g_d4 = g_tracks && g_tracks == g_eng + 0x429C8;
    printf("  engine layout              -> tracks %08X, mixer flag %08X%s\n", g_tracks, g_armed,
           g_d4 ? " (PXD32D4)" : "");
}
#define TRACK(n) (g_tracks + (uint32_t)(n) * 0x84)

static int browser_input(HWND h, UINT m, WPARAM w, LPARAM l);

/* ---- the window the graphics DLL draws into ---------------------------- */
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    /* The graphics DLL owns this client area: it GetDC()s the window and blits
     * straight into it, so it must not be erased behind it. */
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT && g_screen && g_screen->hdc) {
        static PAINTSTRUCT *ps;
        if (!ps) ps = (PAINTSTRUCT *)gmem(sizeof(*ps));
        HDC dc = BeginPaint(h, ps);
        BitBlt(dc, 0, 0, g_screen->w, g_screen->h,
               g_canvas ? g_canvas : (HDC)(INT_PTR)g_screen->hdc, 0, 0, SRCCOPY);
        EndPaint(h, ps);
        return 0;
    }
    if (browser_input(h, m, w, l)) return 0;
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

/* ---- engine state, for --verbose and --tracks ----------------------------
 * The track records live at base+0x429C8, 0x84 apart. +0x6A is how many
 * entries APlay has appended, +0x68 the cursor the mixer compares against it,
 * +0x60 the word AStart's worker resets, +0x64 the entry array - and each entry
 * is 0x108 bytes with start at +0 and end at +4. Reading them back is the only
 * way to see what the engine thinks it has been given. */
static void tracks(const char *when, int n)
{
    if (!g_tracks) return;
    if (g_d4) printf("  [%s] transport %d of %d\n", when, AT(g_eng + 0x3A0C0, int32_t), AT(g_eng + 0x3B150, int32_t));
    for (int t = 0; t < n; t++) {
        gva tr = TRACK(t);
        int count = AT(tr + 0x6A, int16_t);
        if (!count) continue;
        gva ents = AT(tr + 0x64, uint32_t);
        printf("    track %d: count %d cursor %d w60 %d entries %08X\n", t, count,
               AT(tr + 0x68, int16_t), AT(tr + 0x60, int16_t), ents);
        for (int e = 0; ents && e < count && e < 4; e++) {
            const char *en = (const char *)HOSTP(ents + e * 0x108);
            printf("      entry %d: start %d end %d id %d\n", e,
                   *(const int32_t *)en, *(const int32_t *)(en + 4), *(const int16_t *)(en + 0x10));
        }
    }
}

/* 0x4342C is the handshake AStart spins on; 0x434A8 the gate AStart opens and
 * AStop shuts; 0x3AC68 the flag the mixer will not run without; 0x433EC set by
 * DStart once DirectSound is live. */
static void peek(const char *when)
{
    if (!g_d4) return;
#define ENG(off, T) AT(g_eng + (uint32_t)(off), T)
    printf("  [%-12s] ready=%08X astart=%d placed=%d | mixgate=%d armed=%d "
           "dslive=%d crit=%d mode=%d\n", when,
           ENG(0x434A0, uint32_t), ENG(0x4342C, int32_t), ENG(0x429C8 + 0x6A, int16_t),
           ENG(0x434A8, int16_t), ENG(0x3AC68, int16_t), ENG(0x433EC, int16_t),
           ENG(0x4336C, int32_t), ENG(0x39DA4, int16_t));
#undef ENG
}

/* ---- the sample library ---------------------------------------------------
 * eJay does not walk its own disc looking for samples - it ships an index:
 *
 *   DMACHINE\PXD.TXT   18 quoted numbers, then four quoted fields per sample:
 *                      size, length in bars, name line 1, name line 2.
 *                      The 18 numbers are nine (start, count) pairs - the nine
 *                      sound groups the category buttons switch between.
 *   DMACHINE\MAX.TXT   one quoted path per sample, same order: "ba\aaaf.pxd".
 *
 * eJay 2's library is on its second disc. This is eJay 1's, and the 1999 engine
 * plays it without being asked to do anything special about it. */
typedef struct {
    char path[MAX_PATH];        /* its file, or the archive it is in */
    char l1[28], l2[28];
    int  bars, len;             /* len: decoded 16-bit mono bytes, from the header (eJay 1) */
    int  off, size;             /* where it sits in an eJay 2 archive; 0, 0 for a file of its own */
    char name[12];              /* an archive member's own short name, D5MA060 */
} SAMPLE;

#define MAX_GROUPS 12
#define MAX_LIB    12000                /* HipHop eJay 2: 4,034 in its main library alone */
static SAMPLE *g_lib;
static int     g_lib_n;
static int     g_grp_start[MAX_GROUPS], g_grp_count[MAX_GROUPS], g_grp_n;

static int g_group  = 0;    /* which sound group is showing   */
static int g_scroll = 0;    /* first visible row within it    */
static int g_sel    = -1;   /* absolute index of the selection */

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
    FILE *f = xfopen(path, "rb");
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

/* The unit APlay's start and length are in, measured rather than guessed: the
 * transport advances 176,400 per second of playback - 44,100 frames of 16-bit
 * stereo, so an output byte - and eJay's tempo is a fixed 140 BPM, so a 4/4
 * bar is 1.714 seconds: 302,400. (88,200, 120 BPM in samples, was wrong in
 * both, and made every arrangement fire and die in its first few seconds.) */
#define BAR_UNITS 302400

/* How long a sample is, from its own header: the tag, a Pascal-string name, a
 * 0x54 byte, then the decoded length as a byte count of 16-bit mono PCM -
 * 151,200 for a one-bar loop and 37,800 for a one-beat hit, both exact at 140
 * BPM. PXD.TXT's bar count is not reliable enough to time an arrangement. */
static int pxd_length(const char *path)
{
    unsigned char h[400];               /* names run to 72 characters */
    FILE *f = xfopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(h, 1, sizeof(h), f);
    fclose(f);
    if (n < 16 || memcmp(h, "tPxD", 4)) return 0;
    unsigned k = 5u + h[4];             /* past the tag and the name */
    if (k + 5 > n || h[k] != 0x54) return 0;
    return (int)(h[k+1] | (h[k+2] << 8) | (h[k+3] << 16) | ((unsigned)h[k+4] << 24));
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
    if (!g_lib) g_lib = (SAMPLE *)gmem(sizeof(SAMPLE) * MAX_LIB);

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
    while (q && g_lib_n < MAX_LIB) {
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
        /* Whole bars, rounded up: the end an entry is given is also when the
         * lane is free again, and rounding down would cut the tail off every
         * loop not exactly on the bar. A quarter-bar hit still occupies one. */
        sm->bars = sm->len ? (sm->len * 2 + BAR_UNITS - 1) / BAR_UNITS : atoi(bars);
        if (sm->bars < 1 || sm->bars > 16) sm->bars = 1;
        g_lib_n++;
    }
    free(idx); free(files);
    return g_lib_n;
}

/* An eJay 2-generation library: an archive of concatenated tPxD samples and
 * an .INF beside it - DANCE20 and DANCE20.INF, plus a kit or three
 * (DANCESK4.INF ...) in the same folder. The .INF is thirteen header lines
 * and `[SAMPLES]`, then twelve lines a sample:
 *
 *     id, 1, "short name", offset, size, "line 1", "line 2", bars, group, "", 0, ""
 *
 * Every offset lands on a tPxD header (all 1,957 of Dance eJay 2's, checked),
 * and the bars field is the block's length - 1, 2, 3, 4, 6 or 8. APlay is
 * handed the archive with the offset and size as its fourth and fifth
 * arguments, as Dancejay's placement sites do.
 *
 * The group is the button: in Dance eJay 2, 1 to 10 are Loop, Drum, Bass,
 * Guitar, Seq, Layer, Rap, Voice, Effect, Xtra - the category line in each
 * sample's own header says so for every one - and 13 to 18 (Kick, Snare,
 * HiHat, Cymbal, Perc, DrumFx) are the groove generator's, not the browser's.
 * So group n goes to button n, and a group a library lacks stays an empty
 * button rather than moving the others along. */
static int load_inf(const char *root)
{
    char names[16][64];
    int n = plat_list(root, ".INF", names, 16), before = g_lib_n;
    if (!n) return 0;
    if (!g_lib) g_lib = (SAMPLE *)gmem(sizeof(SAMPLE) * MAX_LIB);
    /* gather by group: the browser shows one group's run of the list */
    static int grp_of[MAX_LIB];
    for (int f = 0; f < n; f++) {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\%s", root, names[f]);
        char *txt = slurp(path, NULL);
        const char *p = txt ? strstr(txt, "[SAMPLES]") : NULL;
        if (!p) { free(txt); continue; }
        char archive[MAX_PATH];
        snprintf(archive, sizeof(archive), "%s\\%.*s", root, (int)(strlen(names[f]) - 4), names[f]);
        p += 9;
        while (g_lib_n < MAX_LIB) {
            char fld[12][64];
            int k = 0;
            for (; k < 12; k++) {                       /* one line each */
                while (*p == '\r' || *p == '\n') p++;
                if (!*p) break;
                int m = 0;
                while (*p && *p != '\r' && *p != '\n') { if (*p != '"' && m < 63) fld[k][m++] = *p; p++; }
                fld[k][m] = 0;
            }
            if (k < 12) break;
            SAMPLE *sm = &g_lib[g_lib_n];
            snprintf(sm->path, sizeof(sm->path), "%s", archive);
            snprintf(sm->l1, sizeof(sm->l1), "%s", fld[5]);
            snprintf(sm->l2, sizeof(sm->l2), "%s", fld[6]);
            sm->off = atoi(fld[3]); sm->size = atoi(fld[4]);
            sm->bars = atoi(fld[7]);
            if (sm->bars < 1 || sm->bars > 16) sm->bars = 1;
            sm->len = 0;
            snprintf(sm->name, sizeof(sm->name), "%s", fld[2]);
            grp_of[g_lib_n++] = atoi(fld[8]);
        }
        free(txt);
    }
    /* Sort by group (stable, so each group keeps the .INF's order), then
     * record each group's run as the eJay 1 index does. */
    int cnt = g_lib_n - before;
    SAMPLE *tmp = (SAMPLE *)malloc(sizeof(SAMPLE) * (size_t)cnt);
    int *gt = (int *)malloc(sizeof(int) * (size_t)cnt), at = 0;
    g_grp_n = 0;
    for (int g = 1; g <= MAX_GROUPS; g++) {
        int start = at;
        for (int i = 0; i < cnt; i++)
            if (grp_of[before + i] == g) { tmp[at] = g_lib[before + i]; gt[at++] = g; }
        g_grp_start[g - 1] = before + start;
        g_grp_count[g - 1] = at - start;
        if (at > start) g_grp_n = g;
    }
    memcpy(g_lib + before, tmp, sizeof(SAMPLE) * (size_t)at);
    g_lib_n = before + at;
    free(tmp); free(gt);
    return g_lib_n - before;
}

/* ---- the names on the blocks --------------------------------------------
 * A .MIX is a saved arrangement. Every sample it places is stored as
 *
 *     01 <id:16> <len:16> <name> 00 ...        len = strlen + 2
 *
 * which is enough to read the names back without decoding the rest. */
#define MAX_MIX_NAMES 64
static char g_names[MAX_MIX_NAMES][32];
static int  g_name_count;

static int load_mix_names(const char *path)
{
    long n = 0;
    unsigned char *d = (unsigned char *)slurp(path, &n);
    if (!d) return 0;
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

/* ---- the track grid -------------------------------------------------------
 * `G_SPUREN_SAMPLE_640` in eJay's own control table puts the block area at
 * y 18..298; grid_geometry() replaces these with the table's numbers once
 * K_640 is loaded, and they are the fallback for a run without it. Sixteen
 * lanes, because B_LAUT_01L through 16 is how many volume controls the table
 * carries. The 1999 engine does not draw the playback cursor (eJay 2 draws it
 * in Visual Basic with MoveToEx/LineTo), so this host does. */
static RECT g_grid = { 48, 16, 596, 323 };
#define GRID_X0  ((int)g_grid.left)
#define GRID_X1  ((int)g_grid.right)
#define GRID_Y0  ((int)g_grid.top)
#define GRID_Y1  ((int)g_grid.bottom)
#define GRID_LANES 16
/* Sixteen bars across the window. eJay's grid scrolls (`K_SPUR_HSCROLL`), so
 * this is how much of the song is visible, not how long a song can be. */
#define GRID_BARS 16

/* Bar to pixel, in one place, so the blocks and the playhead agree. */
static int bar_x(double bar) { return GRID_X0 + (int)((GRID_X1 - GRID_X0) * bar / GRID_BARS + 0.5); }
static int lane_y(int lane)  { return GRID_Y0 + (int)((double)(GRID_Y1 - GRID_Y0) * lane / GRID_LANES + 0.5); }

#define BROWSE_X0 176
#define BROWSE_X1 539
#define BROWSE_Y0 366
#define BROWSE_Y1 472

/* One arrangement, used twice: APlay places these on the engine's tracks and
 * GFX_SampleZeichne draws the same list into the same lanes, so what is on the
 * screen is what is playing. */
typedef struct { int lane, bar, bars, lib; } SLOT;
static SLOT g_song[64];
static int  g_song_n;

/* The demo arrangement as written: a lane, a bar, and which sound group to
 * take the sample from - a beat with something over it. */
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

static int g_only = -1;          /* --sample N: that library sample alone, at bar 0 */

static void build_song(void)
{
    g_song_n = 0;
    if (g_only >= 0 && g_only < g_lib_n) {
        g_song[0].lane = 0; g_song[0].bar = 0; g_song[0].lib = g_only; g_song[0].bars = g_lib[g_only].bars;
        g_song_n = 1;
        return;
    }
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
 * `K_640` is the coordinate table: a control name, then ten numbers.
 *
 *     name   dx dy   s1x s1y   s2x s2y   s3x s3y   w h
 *
 * `dx,dy` is where the control sits and `w,h` how big it is; the three source
 * pairs are its states cut out of `EJAY02` - normal, rolled over, pressed.
 * Every number is in a 1280x960 space, so the 640x480 art set halves them. */
#define KSCALE 2

typedef struct {
    char name[32];
    int  x, y, w, h;
    int  sx[3], sy[3];
    int  raw[10];        /* unscaled, for the calls that want them as written */
    int  live;           /* on the page showing (SEITEN), so it answers the mouse */
} KCTRL;

static KCTRL g_ctrl[640];
static int   g_ctrl_n;

static int load_keys(const char *path)
{
    char *buf = slurp(path, NULL);
    if (!buf) return 0;
    char *p = buf;
    g_ctrl_n = 0;
    while (*p && g_ctrl_n < (int)(sizeof(g_ctrl) / sizeof(g_ctrl[0]))) {
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

/* K_640 holds every page's controls, and the drum pads (`:Drumpads`) sit
 * right over the track grid. SEITEN says which controls are on which page -
 * a `:Name` line, then the page's bitmaps and controls - so only `page`'s
 * answer the mouse. Without SEITEN, all of them do. Case is the title's own:
 * Dance eJay 2 ships SEITEN, HipHop eJay 2 seiten; xfopen matches either. */
static void mark_page(const char *page)
{
    char *buf = slurp("SEITEN", NULL);
    int on = 0, n = 0;
    if (!buf) { for (int i = 0; i < g_ctrl_n; i++) g_ctrl[i].live = 1; return; }
    for (char *line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        if (line[0] == ':') {
            char *e = line + 1 + strlen(line + 1);
            while (e > line + 1 && e[-1] == ' ') *--e = 0;
            on = !strcmp(line + 1, page);
            continue;
        }
        if (!on) continue;
        char name[32];
        int k = 0;
        while (line[k] && line[k] != ' ' && k < 31) { name[k] = line[k]; k++; }
        name[k] = 0;
        for (int i = 0; i < g_ctrl_n; i++)
            if (!strcmp(g_ctrl[i].name, name)) { g_ctrl[i].live = 1; n++; }
    }
    free(buf);
    printf("  page %-21s -> %d controls\n", page, n);
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
        if (c->name[0] != 'B' || c->name[1] != '_' || !c->live) continue;
        if (!c->sx[1] && !c->sy[1]) continue;        /* no rollover state */
        if (x >= c->x && x < c->x + c->w && y >= c->y && y < c->y + c->h)
            return i;
    }
    return -1;
}

/* Blit one state of one control from the EJAY02 sheet. */
static void draw_ctrl(HDC dst, const KCTRL *c, int state)
{
    if (!c || !g_sheet || !g_sheet->hdc) return;
    if (state && !c->sx[state] && !c->sy[state]) state = 0;
    if (state == 0) {
        /* state 0 is simply what the chrome already has */
        if (g_screen->hdc)
            BitBlt(dst, c->x, c->y, c->w, c->h, (HDC)(INT_PTR)g_screen->hdc, c->x, c->y, SRCCOPY);
        return;
    }
    BitBlt(dst, c->x, c->y, c->w, c->h, (HDC)(INT_PTR)g_sheet->hdc,
           c->sx[state], c->sy[state], SRCCOPY);
}

/* ---- the browser ---------------------------------------------------------
 * The bottom-centre panel is `G_SAMPLE_WINDOW` in eJay's layout table, with
 * `K_SAMPLE_VSCROLL` down its right edge and twelve `B_GRUPPE_*` buttons either
 * side of it - six left, six right. eJay 1's library has nine sound groups,
 * so nine of the twelve do something. */
#define ROW_H     11
#define BTN_W     49
#define BTN_H     16
#define BTN_LX   112
#define BTN_RX   569
#define BTN_Y0   346
#define BTN_PITCH 22
#define SCROLL_X 543

static RECT g_browse = { BROWSE_X0, BROWSE_Y0, BROWSE_X1, BROWSE_Y1 };
static int  g_scroll_x = SCROLL_X, g_scroll_w = 9;

static int browser_rows(void);

static void grid_geometry(void)
{
    const KCTRL *c = find_ctrl("G_SPUREN_SAMPLE_640");
    if (!c || c->w <= 0) c = find_ctrl("G_SPUREN_SAMPLE");
    if (!c || c->w <= 0 || c->h <= 0) return;
    g_grid.left = c->x; g_grid.top = c->y;
    g_grid.right = c->x + c->w; g_grid.bottom = c->y + c->h;
    printf("  track grid                 -> x %d..%d, y %d..%d, %d lanes of %d px,"
           " %d bars of %d px\n", GRID_X0, GRID_X1, GRID_Y0, GRID_Y1,
           GRID_LANES, lane_y(1) - lane_y(0), GRID_BARS, bar_x(1) - bar_x(0));
}

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

/* G_SAMPLE_WINDOW includes its bezel; text at its very edges sits on the frame. */
#define BEZEL_T 7
#define BEZEL_B 6
#define BEZEL_L 6

static int browser_rows(void)
{
    return (int)(g_browse.bottom - g_browse.top - BEZEL_T - BEZEL_B) / ROW_H;
}

/* Draw every placed sample into the lanes: at start-up and whenever the
 * arrangement changes, so the grid always shows the list the engine plays. */
static void draw_blocks(HDC dst)
{
    if (!g_zeich || !g_griddc || !g_screen || !g_screen->hdc) return;
    BitBlt(dst, GRID_X0, GRID_Y0, GRID_X1 - GRID_X0, GRID_Y1 - GRID_Y0,
           (HDC)(INT_PTR)g_screen->hdc, GRID_X0, GRID_Y0, SRCCOPY);
    int lane_h = lane_y(1) - lane_y(0);
    for (int i = 0; i < g_song_n; i++) {
        const SLOT *sl = &g_song[i];
        int x0 = bar_x(sl->bar), w = bar_x(sl->bar + sl->bars) - x0;
        int style = g_tex[i % 3];
        const SAMPLE *sm = (sl->lib >= 0 && sl->lib < g_lib_n) ? &g_lib[sl->lib] : NULL;
        if (!style || w < 4 || sl->lane >= GRID_LANES) continue;
        X(g_zeich, style, 0, w, GSTR(sm ? sm->l1 : "Sample"), GSTR(sm ? sm->l2 : ""), 0, 0);
        BitBlt(dst, x0, lane_y(sl->lane) + 1, w - 2, lane_h - 2, (HDC)(INT_PTR)g_griddc, 0, 0, SRCCOPY);
    }
}

/* Which category button is under the pointer, or -1: down the left column
 * first, then the right, the order SEITEN lists them in. */
/* Group i's button, from the title's own control table: B_GRUPPE_01 is group
 * 1's, and so on - Dance eJay 2 has six a side, Techno eJay five, HipHop
 * eJay 2 puts 12 above 10 and 11. Zero-sized or absent: no button. Without
 * K_640 at all, Dance eJay 2's measured layout. */
static int group_button(int i, RECT *r)
{
    if (g_ctrl_n) {
        char nm[16];
        snprintf(nm, sizeof(nm), "B_GRUPPE_%02d", i + 1);
        const KCTRL *c = find_ctrl(nm);
        if (!c || c->w <= 0 || c->h <= 0) return 0;
        r->left = c->x; r->top = c->y; r->right = c->x + c->w; r->bottom = c->y + c->h;
        return 1;
    }
    r->left = (i < 6) ? BTN_LX : BTN_RX;
    r->top = BTN_Y0 + (i % 6) * BTN_PITCH;
    r->right = r->left + BTN_W; r->bottom = r->top + BTN_H;
    return 1;
}

static int hit_group(int x, int y)
{
    RECT r;
    for (int i = 0; i < 12; i++)
        if (group_button(i, &r) && x >= r.left && x < r.right && y >= r.top && y < r.bottom) return i;
    return -1;
}

static void draw_browser(HDC dst)
{
    if (!g_lib_n) return;
    int gs = g_grp_start[g_group], gc = g_grp_count[g_group];
    int rows = browser_rows();
    if (g_scroll > gc - rows) g_scroll = gc - rows;
    if (g_scroll < 0) g_scroll = 0;
    if (!dst || !g_screen || !g_screen->hdc) return;   /* clamp only, for the selftest */

    /* Repaint the panel from the chrome first, so a scroll does not smear. */
    BitBlt(dst, (int)g_browse.left, (int)g_browse.top,
           (int)g_browse.right - (int)g_browse.left, (int)g_browse.bottom - (int)g_browse.top,
           (HDC)(INT_PTR)g_screen->hdc, (int)g_browse.left, (int)g_browse.top, SRCCOPY);

    HFONT fnt = (HFONT)FONT(-10, "Small Fonts");
    HGDIOBJ old = SelectObject(dst, fnt);
    SetBkMode(dst, TRANSPARENT);
    /* Clip to the panel: without it the top row is drawn over the transport. */
    HRGN clip = CreateRectRgn((int)g_browse.left + BEZEL_L, (int)g_browse.top + BEZEL_T - 1,
                              (int)g_browse.right - BEZEL_L, (int)g_browse.bottom - BEZEL_B + 1);
    SelectClipRgn(dst, clip);

    for (int r = 0; r < rows && g_scroll + r < gc; r++) {
        int idx = gs + g_scroll + r;
        int y = (int)g_browse.top + BEZEL_T + r * ROW_H;
        const SAMPLE *sm = &g_lib[idx];
        if (idx == g_sel) {
            RECT sel = { (int)g_browse.left + 4, y - 1, (int)g_browse.right - 6, y + ROW_H - 1 };
            HBRUSH hb = CreateSolidBrush(RGB(40, 60, 150));
            FILLRECT(dst, &sel, hb);
            DeleteObject(hb);
        }
        SetTextColor(dst, idx == g_sel ? RGB(255, 255, 255) : RGB(255, 190, 80));
        TEXTOUT(dst, (int)g_browse.left + 8, y, sm->l1);
        SetTextColor(dst, idx == g_sel ? RGB(220, 230, 255) : RGB(160, 180, 240));
        TEXTOUT(dst, (int)g_browse.left + 140, y, sm->l2);
        char bars[16];
        snprintf(bars, sizeof(bars), "%d", sm->bars);
        SetTextColor(dst, RGB(120, 140, 200));
        TEXTOUT(dst, (int)g_browse.left + 250, y, bars);
        const char *file = sm->name[0] ? sm->name : strrchr(sm->path, '\\');
        if (file) TEXTOUT(dst, (int)g_browse.left + 276, y, file + (file != sm->name));
    }

    /* The scroll thumb, sized and placed like the list it stands for. */
    if (gc > rows) {
        int track = (int)g_browse.bottom - (int)g_browse.top - 8;
        int th = track * rows / gc;
        if (th < 8) th = 8;
        int ty = (int)g_browse.top + 4 + (track - th) * g_scroll / (gc - rows);
        RECT thumb = { g_scroll_x, ty, g_scroll_x + 9, ty + th };
        HBRUSH hb = CreateSolidBrush(RGB(120, 150, 230));
        FILLRECT(dst, &thumb, hb);
        DeleteObject(hb);
    }

    SelectClipRgn(dst, 0);
    DeleteObject(clip);
    SelectObject(dst, old);
    DeleteObject(fnt);

    /* Light the live category from the sheet. Two states only - the third pair
     * is 0,0 - so "on" is the second, which is also the roll-over. */
    for (int i = 0; i < 12; i++) {
        char nm[16];
        snprintf(nm, sizeof(nm), "B_GRUPPE_%02d", i + 1);
        const KCTRL *c = find_ctrl(nm);
        if (c) draw_ctrl(dst, c, i == g_group ? 1 : 0);
    }
}

/* ---- dragging a block along its lane -------------------------------------
 * The engine has no "move": APlay appends to a track's fixed array of entries
 * and nothing removes one. So a drop empties the tracks in place (count and
 * cursor to 0) and places every slot again. Not ACloseAll: that closes the
 * bitmaps ALoad made - DeleteDC and DeleteObject over all 200 slots - which
 * is the workspace and the button sheet. The first version of this host did
 * call it here, and every drop quietly destroyed the chrome it repaints from.
 * ponytail: rebuild the whole arrangement on drop. Fine for sixty-four slots;
 * revisit if the engine turns out to have a per-entry edit. */
static int g_hover = -1, g_down = -1;

static void paint_ctrl(HWND wnd, int i, int state)
{
    if (i < 0 || i >= g_ctrl_n || !g_canvas) return;
    HDC dc = GetDC(wnd);
    draw_ctrl(g_canvas, &g_ctrl[i], state);
    BitBlt(dc, g_ctrl[i].x, g_ctrl[i].y, g_ctrl[i].w, g_ctrl[i].h,
           g_canvas, g_ctrl[i].x, g_ctrl[i].y, SRCCOPY);
    ReleaseDC(wnd, dc);
}

static int g_astart = 0xA17FC0;   /* what AStart is told the song is: 4 minutes */
static int g_drag = -1;           /* index into g_song, or -1 */
static int g_drag_dx;             /* grab offset within the block, in bars */
static int16_t *g_status;         /* APlay's status words, one per slot: the engine writes them */

static int bar_width(void) { return bar_x(1) - bar_x(0); }

static int slot_at(int x, int y)
{
    if (x < GRID_X0 || x >= GRID_X1 || y < GRID_Y0 || y >= GRID_Y1) return -1;
    int lane = (y - GRID_Y0) * GRID_LANES / (GRID_Y1 - GRID_Y0);
    int bar  = (x - GRID_X0) * GRID_BARS / (GRID_X1 - GRID_X0);
    for (int i = 0; i < g_song_n; i++)
        if (g_song[i].lane == lane && bar >= g_song[i].bar &&
            bar < g_song[i].bar + g_song[i].bars) return i;
    return -1;
}

/* The file APlay is given for a sample. A sample of its own (eJay 1's) is
 * its own file. An archive member is copied out once, to the cache folder:
 * the engine opens whatever it is given, asks its size and reads it from the
 * start - there is no offset to give it - and each member is a whole tPxD
 * file. Dancejay hands APlay names from a list of its own, presumably the
 * same copies (the engine even exports RWavToTemp). */
static const char *sample_file(const SAMPLE *sm, char *out, size_t n)
{
    if (!sm->size) return sm->path;
    const char *base = strrchr(sm->path, '\\');
    snprintf(out, n, "%s\\%s_%s.PXD", plat_cache(), base ? base + 1 : sm->path, sm->name);
    FILE *f = xfopen(out, "rb");
    if (f) { fclose(f); return out; }
    FILE *a = xfopen(sm->path, "rb");
    char *buf = (char *)malloc((size_t)sm->size);
    int ok = a && buf && fseek(a, sm->off, SEEK_SET) == 0 && fread(buf, 1, (size_t)sm->size, a) == (size_t)sm->size &&
             !memcmp(buf, "tPxD", 4);
    if (a) fclose(a);
    if (ok && (f = xfopen(out, "wb")) != NULL) {
        ok = fwrite(buf, 1, (size_t)sm->size, f) == (size_t)sm->size;
        fclose(f);
    }
    free(buf);
    if (!ok) printf("  ! could not copy %s out of %s\n", sm->name, sm->path);
    return out;
}

/* APlay's twelve arguments (docs/EJAY2.md): 1 a sample id, stored at
 * entry+0x10 (thirteen entries all claiming id 0 sound like one); 6 a status
 * word the engine writes into; 7 the file; 8 the track; 9 the start and 10 the
 * length, in engine units (a length of 0 means "no end", and an entry with no
 * end plays from the moment the transport starts). */
static void place(int id, int slot, const SAMPLE *sm, int lane, int start, int len)
{
    g_status[slot] = 0x63;
    char file[MAX_PATH];
    /* the engine copies the name into its entry (lstrcpyA), so a short-lived
     * copy of it is enough */
    X(g_aplay, id, 0, 0, 0, 0, GVA(&g_status[slot]), GSTR(sample_file(sm, file, sizeof(file))),
      lane, start, len, 0, 0x100);
}

/* Every slot to the engine, earliest first: the mixer walks a track's
 * entries in order, comparing the next one's start with the transport, so an
 * entry placed after a later one would never fire. ids: give each its own
 * sample id (APlay's first argument), as Dancejay's placement sites do. */
static int place_all(int ids)
{
    int order[64], n = 0, placed = 0;
    for (int i = 0; i < g_song_n && i < 64; i++) {
        int k = n++;
        while (k > 0 && g_song[order[k - 1]].bar > g_song[i].bar) { order[k] = order[k - 1]; k--; }
        order[k] = i;
    }
    for (int j = 0; j < n && g_aplay; j++) {
        const SLOT *sl = &g_song[order[j]];
        if (sl->lib < 0 || sl->lib >= g_lib_n) continue;
        place(ids ? order[j] + 1 : 0, order[j], &g_lib[sl->lib], sl->lane, sl->bar * BAR_UNITS, sl->bars * BAR_UNITS);
        placed++;
    }
    return placed;
}

static void rebuild_arrangement(void)
{
    if (!g_tracks) { printf("  ! cannot move blocks: this engine's track records were not found\n"); return; }
    /* AStop asks the mixer to stop, it does not wait for it. Emptying the
     * tracks under a pass still running is a race, so give the audio thread
     * a moment first.
     * ponytail: a fixed wait. A handshake would be better if one exists. */
    X0(eng("AStop"));
    plat_sleep(40);
    for (int t = 0; t < GRID_LANES; t++) {
        AT(TRACK(t) + 0x6A, int16_t) = 0;
        AT(TRACK(t) + 0x68, int16_t) = 0;
    }
    place_all(1);
    X(eng("AStart"), (uint32_t)g_astart);
}

static int browser_input(HWND h, UINT m, WPARAM w, LPARAM l)
{
    /* A block follows the pointer along its own lane, snapped to the bar grid,
     * and the arrangement goes back to the engine when it is dropped. */
    if (m == WM_MOUSEMOVE && g_drag >= 0) {
        SLOT *sl = &g_song[g_drag];
        int bar = ((short)LOWORD(l) - GRID_X0) * GRID_BARS / (GRID_X1 - GRID_X0) - g_drag_dx;
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
               (sl->lib >= 0 && sl->lib < g_lib_n) ? g_lib[sl->lib].l1 : "?", sl->lane, sl->bar);
        rebuild_arrangement();
        g_drag = -1;
        ReleaseCapture();
        return 1;
    }
    if (m == WM_LBUTTONDOWN) {
        int hit = slot_at((short)LOWORD(l), (short)HIWORD(l));
        if (hit >= 0) {
            g_drag = hit;
            g_drag_dx = ((short)LOWORD(l) - GRID_X0) * GRID_BARS / (GRID_X1 - GRID_X0) - g_song[hit].bar;
            SetCapture(h);
            return 1;
        }
    }

    /* Roll-over and press come straight off the sheet: two blits, not a repaint. */
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

    /* A category button switches the sound group, the wheel or a click in the
     * scroll strip moves the list, a click selects a row and a double click
     * places it - eJay's own tooltip: "double click for play back of a sample,
     * move a sample to one of the tracks". */
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
        if (x >= (int)g_browse.left && x < (int)g_browse.right &&
            y >= (int)g_browse.top && y < (int)g_browse.bottom) {
            int r = (y - (int)g_browse.top - BEZEL_T) / ROW_H;
            int idx = g_grp_start[g_group] + g_scroll + r;
            if (r >= 0 && r < browser_rows() && g_scroll + r < g_grp_count[g_group] && idx < g_lib_n) {
                g_sel = idx;
                g_dirty = 1;
                if (m == WM_LBUTTONDBLCLK) g_place = idx;
            }
            return 1;
        }
    }
    return 0;
}

/* ---- reading pixels back --------------------------------------------------
 * A 32-bit top-down DIB section of our own, a DC's picture blitted into it,
 * read in place: the same on Windows and on win32hle, with no GetDIBits.
 * Pixels are 0x00RRGGBB. Release with ungrab(). */
typedef struct {
    uint32_t size; int32_t w, h; uint16_t planes, bpp;
    uint32_t comp, sizeimage; int32_t xppm, yppm; uint32_t used, important;
    uint32_t mask[3];
} BMI;

typedef struct { HDC dc; HBITMAP bm; HGDIOBJ old; const uint32_t *px; } GRAB;

static int grab(GRAB *g, HDC src, int w, int h)
{
    static BMI *bi;
    if (!bi) bi = (BMI *)gmem(sizeof(*bi));
    memset(bi, 0, sizeof(*bi));
    bi->size = 40; bi->w = w; bi->h = -h; bi->planes = 1; bi->bpp = 32;
    gva bits = 0;
    g->bm = dib_section(src, bi, &bits);
    g->dc = CreateCompatibleDC(0);
    if (!g->bm || !g->dc || !bits) {
        printf("  ! read-back of %dx%d failed: bitmap %08X dc %08X bits %08X\n",
               w, h, (unsigned)(uintptr_t)g->bm, (unsigned)(uintptr_t)g->dc, bits);
        return 0;
    }
    g->old = SelectObject(g->dc, g->bm);
    BitBlt(g->dc, 0, 0, w, h, src, 0, 0, SRCCOPY);
    g->px = (const uint32_t *)HOSTP(bits);
    return 1;
}
static void ungrab(GRAB *g)
{
    if (g->dc) { SelectObject(g->dc, g->old); DeleteDC(g->dc); }
    if (g->bm) DeleteObject(g->bm);
}

static int pixel_diff(uint32_t a, uint32_t b)
{
    return abs((int)(a & 255) - (int)(b & 255)) + abs((int)((a >> 8) & 255) - (int)((b >> 8) & 255)) +
           abs((int)((a >> 16) & 255) - (int)((b >> 16) & 255));
}

/* ---- did we paint outside the lines? --------------------------------------
 * Twice the browser list crept up over the panel frame, and both times it was
 * only caught by eye. Compare the window against the chrome it was built
 * from: every pixel that differs must be inside a region we are entitled to
 * paint - the grid, the browser panel, the category buttons. */
static void write_bmp(const char *path, const uint32_t *px, int w, int h);
static int paint_check(HWND wnd, const char *shot)
{
    if (!g_screen || !g_screen->hdc) return 0;
    int w = g_screen->w, h = g_screen->h;
    GRAB a = { 0 };
    HDC wdc = GetDC(wnd);
    /* The chrome straight from ALoad's own pixels: an 8bpp bottom-up DIB and
     * its 256 RGBQUADs, which read as 0x00RRGGBB. (Blitting its DC into a
     * second one comes out black on Windows.) */
    uint32_t *chrome = (uint32_t *)malloc((size_t)w * h * 4);
    const uint8_t *bits = (const uint8_t *)HOSTP(g_screen->bits);
    const uint32_t *pal = (const uint32_t *)HOSTP(g_screen->pal);
    int stride = (w + 3) & ~3;
    for (int y = 0; chrome && bits && pal && y < h; y++)
        for (int x = 0; x < w; x++) chrome[(size_t)y * w + x] = pal[bits[(size_t)(h - 1 - y) * stride + x]] & 0xFFFFFF;
    struct { const uint32_t *px; } b = { chrome };
    int ok = chrome && bits && pal && grab(&a, wdc, w, h);
    long stray = 0;
    int fy = -1, fx = -1;
    for (int y = 0; ok && y < h; y++)
        for (int x = 0; x < w; x++) {
            if (pixel_diff(a.px[y * w + x], b.px[y * w + x]) <= 30) continue;
            int inside =
                (y >= GRID_Y0 && y < GRID_Y1 && x >= GRID_X0 && x < GRID_X1) ||
                (y >= (int)g_browse.top && y < (int)g_browse.bottom &&
                 x >= (int)g_browse.left && x < g_scroll_x + g_scroll_w + 2);
            RECT r;
            for (int i = 0; !inside && i < 12; i++)        /* the category buttons, lit or not */
                inside = group_button(i, &r) && x >= r.left - 1 && x <= r.right && y >= r.top - 1 && y <= r.bottom;
            if (!inside) { if (fy < 0) { fy = y; fx = x; } stray++; }
        }
    if (ok && stray && shot) {                   /* both pictures, to see what differs */
        char f[MAX_PATH];
        snprintf(f, sizeof(f), "%s.window.bmp", shot);  write_bmp(f, a.px, w, h);
        snprintf(f, sizeof(f), "%s.chrome.bmp", shot);  write_bmp(f, b.px, w, h);
    }
    ungrab(&a);
    free(chrome);
    ReleaseDC(wnd, wdc);
    if (!ok) { printf("  paint check                -> could not read back\n"); return 0; }
    if (stray) printf("  paint check                -> %ld px outside, first at %d,%d\n", stray, fx, fy);
    else printf("  paint check                -> ok, nothing outside the panels\n");
    return stray != 0;
}

/* The hit-testing and the scroll clamp are the only real logic in here, so
 * they get the one check: synthetic coordinates, no mouse. --selftest. */
static int browser_selftest(void)
{
    int bad = 0;
    RECT r;
    for (int i = 0; i < 12; i++) {
        if (!group_button(i, &r)) continue;
        int got = hit_group((int)(r.left + r.right) / 2, (int)(r.top + r.bottom) / 2);
        if (got != i) { printf("  ! button %d hit-tests as %d\n", i, got); bad++; }
    }
    if (hit_group((int)g_browse.left + 40, (int)g_browse.top + 40) != -1) {
        printf("  ! a click in the list hit-tests as a button\n"); bad++;
    }
    if (g_lib_n) {
        int save_g = g_group, save_s = g_scroll;
        g_group = 0;
        g_scroll = 1000000;                  /* far past the end */
        draw_browser(0);                     /* clamps as a side effect */
        if (g_scroll != g_grp_count[0] - browser_rows()) {
            printf("  ! scroll clamped to %d, expected %d\n", g_scroll, g_grp_count[0] - browser_rows());
            bad++;
        }
        g_scroll = -5;
        draw_browser(0);
        if (g_scroll != 0) { printf("  ! negative scroll not clamped\n"); bad++; }
        g_group = save_g; g_scroll = save_s;
    }
    printf("  browser selftest           -> %s\n", bad ? "FAILED" : "ok");
    return bad;
}

static void draw_cursor(HWND wnd, double frac)
{
    static int last = -1;
    if (!g_screen || !g_screen->hdc) return;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    g_cursor_frac = frac;
    int x = bar_x(frac * GRID_BARS);
    HDC dc = GetDC(wnd);
    /* Erase by copying the arrangement back, or the cursor smears into a bar. */
    if (last >= 0)
        BitBlt(dc, last, GRID_Y0, 2, GRID_Y1 - GRID_Y0,
               g_canvas ? g_canvas : (HDC)(INT_PTR)g_screen->hdc, last, GRID_Y0, SRCCOPY);
    PatBlt(dc, x, GRID_Y0, 2, GRID_Y1 - GRID_Y0, WHITENESS);
    ReleaseDC(wnd, dc);
    last = x;
}

/* A 24-bit bottom-up BMP of 0x00RRGGBB pixels, top row first. */
static void write_bmp(const char *path, const uint32_t *px, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    int stride = (w * 3 + 3) & ~3;
    uint32_t data = (uint32_t)stride * h;
    unsigned char hdr[54] = { 'B', 'M' };
    uint32_t v[] = { 54 + data, 0, 54, 40, (uint32_t)w, (uint32_t)h };
    memcpy(hdr + 2, v, sizeof(v));
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, sizeof(hdr), f);
    unsigned char *row = (unsigned char *)calloc(1, (size_t)stride);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            uint32_t c = px[(size_t)y * w + x];
            row[x * 3] = (unsigned char)c; row[x * 3 + 1] = (unsigned char)(c >> 8); row[x * 3 + 2] = (unsigned char)(c >> 16);
        }
        fwrite(row, 1, (size_t)stride, f);
    }
    free(row);
    fclose(f);
    printf("  wrote %s\n", path);
}

/* Save the window as a BMP and count what is not black. "The DLL returned 1"
 * and "the DLL drew something" are different claims; only the second counts. */
static int capture(HWND wnd, int w, int h, const char *path)
{
    GRAB g = { 0 };
    HDC wdc = GetDC(wnd);
    long lit = 0;
    if (grab(&g, wdc, w, h)) {
        for (long i = 0; i < (long)w * h; i++) if (g.px[i] & 0xFFFFFF) lit++;
        if (path) write_bmp(path, g.px, w, h);
    }
    ungrab(&g);
    ReleaseDC(wnd, wdc);
    printf("  window: %ld of %ld px non-black\n", lit, (long)w * h);
    return lit > 0;
}

/* Load one bitmap through the engine's own loader and report what came back. */
static int load_bitmap(gva ALoad, ALOADREC *rec, const char *name)
{
    memset(rec, 0, sizeof(*rec));
    /* VB pads this field with spaces; a plain NUL-terminated name works too. */
    snprintf(rec->name, sizeof(rec->name), "%s", name);
    X(ALoad, GVA(rec));
    printf("  ALoad(%-18s) -> hdc %08X  %dx%d  bits %08X\n", name, rec->hdc, rec->w, rec->h, rec->bits);
    return rec->hdc != 0;
}

/* The 16-bit 5-6-5 scratch surface GFX_IntroInitScreenCopy wants: its blend
 * loop ends `mov word ptr [edi], ax`, so an 8bpp bitmap would give it half the
 * buffer it believes it has. */
static int copy_surface(HWND wnd, int w, int h, gva *bits)
{
    BMI *bi = (BMI *)gmem(sizeof(BMI));
    bi->size = 40; bi->w = w; bi->h = h; bi->planes = 1; bi->bpp = 16;
    bi->comp = 3;                                  /* BI_BITFIELDS */
    bi->mask[0] = 0xF800; bi->mask[1] = 0x07E0; bi->mask[2] = 0x001F;
    HDC wdc = GetDC(wnd);
    HBITMAP cbm = dib_section(wdc, bi, bits);
    HDC cdc = CreateCompatibleDC(wdc);
    if (cbm && cdc) SelectObject(cdc, cbm);
    ReleaseDC(wnd, wdc);
    printf("  copy surface               -> %dx%d 16bpp, bits %08X\n", w, h, *bits);
    return (int)cdc;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* a crash must not eat the trail */
    if (plat_open(argc, argv)) return 1;
    int ticks = 120, volume = 20, atyp = 3, dplay = 0, chan = 9;
    int intro = 1, frames = 0, verbose = 0, main_screen = 0, samples = 0;
    int seq = 0, song = 0, selftest = 0, dragtest = 0, paintcheck = 0;
    int playpos = 0, playlen = 0, onset = 0, latency = 0, ids = 0, trackdump = 0;
    const char *libdir = NULL;
    /* The SAMPLE block of FONTS reads: Small Fonts / normal / 10 / 1 / -1 / 6. */
    int fontsize = 10, face_a = 1, face_b = -1, face_c = 6;
    const char *gfxdir = "GRAFIKA";
    const char *palname = "DANCE2";
    const char *shot = NULL;
    /* METRO.PXD, the metronome, is the only real sample on the install disc.
     * DINTRO.PXD is a mix, not a sample: DPlayFile renders it as static. */
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
        else if (!strcmp(argv[i], "--song")) { song = seq = samples = main_screen = 1; intro = 0; }
        else if (!strcmp(argv[i], "--selftest")) selftest = 1;
        else if (!strcmp(argv[i], "--dragtest")) dragtest = 1;
        else if (!strcmp(argv[i], "--paintcheck")) paintcheck = 1;
        else if (!strcmp(argv[i], "--pos") && i + 1 < argc) playpos = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ids")) ids = 1;
        else if (!strcmp(argv[i], "--tracks")) trackdump = 1;
        else if (!strcmp(argv[i], "--astart") && i + 1 < argc) g_astart = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--file") && i + 1 < argc) { playfile = argv[++i]; seq = 1; }
        else if (!strcmp(argv[i], "--len") && i + 1 < argc) playlen = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--onset") && i + 1 < argc) onset = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--latency") && i + 1 < argc) latency = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--engine") && i + 1 < argc) g_engine_name = argv[++i];
        else if (!strcmp(argv[i], "--gfxdll") && i + 1 < argc) g_gfx_name = argv[++i];
        else if (!strcmp(argv[i], "--pal") && i + 1 < argc) palname = argv[++i];
        else if (!strcmp(argv[i], "--group") && i + 1 < argc) g_group = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sample") && i + 1 < argc) g_only = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-intro")) intro = 0;
        else if (!strcmp(argv[i], "--main")) { main_screen = 1; intro = 0; }
        else if (!strcmp(argv[i], "--samples")) { samples = main_screen = 1; intro = 0; }
        else if (!strcmp(argv[i], "--mix") && i + 1 < argc) mixfile = argv[++i];
        else if (!strcmp(argv[i], "--font") && i + 4 < argc) {
            fontsize = atoi(argv[++i]); face_a = atoi(argv[++i]);
            face_b = atoi(argv[++i]); face_c = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--verbose")) verbose = 1;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
    }
    if (plat_headless() && !ticks) ticks = 600;   /* no window to close */

    printf("eJay 2 host - the title's own engine and graphics DLLs\n\n");
    g_eng = load_dll(g_engine_name);
    g_gfx = load_dll(g_gfx_name);
    if (!g_eng) { printf("cannot load %s\n", g_engine_name); return 1; }
    printf("  %s at %08X\n", g_engine_name, g_eng);
    if (g_gfx) printf("  %s at %08X\n", g_gfx_name, g_gfx);
    find_layout();
    g_screen = (ALOADREC *)gmem(sizeof(ALOADREC));
    g_sheet  = (ALOADREC *)gmem(sizeof(ALOADREC));
    g_status = (int16_t *)gmem(sizeof(int16_t) * 66);

    if (libdir) {
        libdir = plat_lib_root(libdir);
        int n = load_index(libdir);
        if (!n) n = load_inf(libdir);
        printf("  sample library             -> %d samples in %d groups under %s\n", n, g_grp_n, libdir);
        build_song();
        printf("  arrangement                -> %d slots\n", g_song_n);
    }
    plat_audio_guard(volume);

    HWND wnd = plat_window(640, 480, "eJay 2", wndproc);
    printf("  window %08X\n", (unsigned)(uintptr_t)wnd);

    gva ALoad    = need(g_eng, "ALoad");
    gva AFenster = eng("AFenster");
    gva ATimer   = eng("ATimer");
    gva ALautSet = eng("ALautSet");

    /* ---- Dancejay.exe's own order, read off its call sites --------------
     * ADevice probes formats and returns a tier, not a device count: 0 means
     * its first probe (44100Hz 16-bit stereo) was accepted. AInit likewise
     * returns the engine's error word, so 0 is success. */
    printf("  ATyp(%d)                    -> %d\n", atyp, (int)X(need(g_eng, "ATyp"), (uint32_t)atyp));
    printf("  ADevice(0)                 -> %d (0 = 44.1k/16/stereo accepted)\n", (int)X(need(g_eng, "ADevice"), 0));
    printf("  AInit(hwnd)                -> %d (0 = no error)\n", (int)X(need(g_eng, "AInit"), (uint32_t)(uintptr_t)wnd));
    if (verbose) peek("after AInit");

    gva SetActive = gfx("GFX_SetActiveWindow"), SwapInit = gfx("GFX_AnimationSwapInit");
    if (SetActive) printf("  GFX_SetActiveWindow(hwnd)  -> %d\n", (int)X(SetActive, (uint32_t)(uintptr_t)wnd));
    if (SwapInit)  printf("  GFX_AnimationSwapInit      -> %08X\n", X(SwapInit, 400, 400));

    /* ---- the intro screen ---------------------------------------------
     * SEITEN lists three bitmaps under `:Intro` - EJAY31, EJAY33, EJAY32 - and
     * the graphics DLL wants them as live DIBs, which is what ALoad returns. */
    gva InitScreen = gfx("GFX_IntroInitScreen"), InitCopy = gfx("GFX_IntroInitScreenCopy");
    gva InitLeds = gfx("GFX_IntroInitLeds"), InitText = gfx("GFX_IntroInitText");
    gva Refresh = gfx("GFX_IntroRefresh"), SetKey = gfx("GFX_IntroSetKey");
    if (intro && ALoad && InitScreen) {
        ALOADREC *scr = (ALOADREC *)gmem(sizeof(ALOADREC));
        ALOADREC *leds = (ALOADREC *)gmem(sizeof(ALOADREC));
        ALOADREC *text = (ALOADREC *)gmem(sizeof(ALOADREC));
        char path[MAX_PATH];

        /* Register the keys before the bitmaps. K_640 holds 58 `K_INTRO_*`
         * entries and GFX_IntroSetKey files each one's rect by name: x, y, w,
         * h first - in a K_640 record the size is the LAST pair - then the
         * three source pairs. Done after the bitmaps, the fill routine walks a
         * zero rectangle, and the animation stalls for 92 seconds. */
        int keys = 0;
        if (SetKey && load_keys("K_640")) {
            for (int i = 0; i < g_ctrl_n; i++) {
                const KCTRL *c = &g_ctrl[i];
                if (strncmp(c->name, "K_INTRO", 7)) continue;
                const int *n = c->raw;
                X(SetKey, GSTR(c->name), n[0] / KSCALE, n[1] / KSCALE, n[8] / KSCALE, n[9] / KSCALE,
                  n[2] / KSCALE, n[3] / KSCALE, n[4] / KSCALE, n[5] / KSCALE, n[6] / KSCALE, n[7] / KSCALE);
                keys++;
            }
        }
        printf("  GFX_IntroSetKey x%d\n", keys);

        snprintf(path, sizeof(path), "%s\\EJAY31A", gfxdir);
        if (load_bitmap(ALoad, scr, path)) {
            printf("  GFX_IntroInitScreen        -> %d\n",
                   (int)X(InitScreen, (uint32_t)scr->hdc, (uint32_t)scr->w, (uint32_t)scr->h, scr->bits, scr->pal));
            /* The copy surface has to be real: InitScreenCopy bails on a null
             * pixel pointer before it stores the DC the animation later blits
             * from, which is a crash waiting at t > 3400ms. */
            if (InitCopy) {
                gva cbits = 0;
                int cdc = copy_surface(wnd, scr->w, scr->h, &cbits);
                printf("  GFX_IntroInitScreenCopy    -> %d\n",
                       (int)X(InitCopy, (uint32_t)cdc, (uint32_t)scr->w, (uint32_t)scr->h, cbits));
            }
        } else {
            /* HipHop eJay 2 ships no intro bitmaps - it has no system check -
             * and running the animation with nothing to draw from faults
             * inside the DLL's own refresh. */
            printf("  ! no intro bitmaps in %s - skipping the system check\n", gfxdir);
            intro = 0;
        }
        snprintf(path, sizeof(path), "%s\\EJAY33A", gfxdir);
        if (intro && InitLeds && load_bitmap(ALoad, leds, path))
            printf("  GFX_IntroInitLeds          -> %d\n",
                   (int)X(InitLeds, (uint32_t)leds->hdc, (uint32_t)leds->w, (uint32_t)leds->h, leds->bits));
        snprintf(path, sizeof(path), "%s\\EJAY32A", gfxdir);
        if (intro && InitText && load_bitmap(ALoad, text, path))
            printf("  GFX_IntroInitText          -> %d\n",
                   (int)X(InitText, (uint32_t)text->hdc, (uint32_t)text->w, (uint32_t)text->h, text->bits));
        /* Not Refresh(-1): that is the ending, not an init - its branch
         * switches every later call to the closing renderer. */
    }

    /* ---- the workspace -------------------------------------------------
     * SEITEN calls this page `:Hauptbild` and gives it two bitmaps: EJAY01 is
     * the 640x480 chrome, EJAY02 the sheet every button state is cut from. */
    if (main_screen && ALoad) {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\EJAY01A", gfxdir);
        if (load_bitmap(ALoad, g_screen, path)) {
            HDC dc = GetDC(wnd);
            BitBlt(dc, 0, 0, g_screen->w, g_screen->h, (HDC)(INT_PTR)g_screen->hdc, 0, 0, SRCCOPY);
            ReleaseDC(wnd, dc);
        }
        snprintf(path, sizeof(path), "%s\\EJAY02A", gfxdir);
        load_bitmap(ALoad, g_sheet, path);
        printf("  K_640 controls             -> %d\n", load_keys("K_640"));
        mark_page("Hauptbild");
        browser_geometry();
        grid_geometry();
    }
    if (mixfile) printf("  %s -> %d sample names\n", mixfile, load_mix_names(mixfile));

    /* ---- the arrangement -----------------------------------------------
     * The order matters and is the opposite of the obvious one: GFX_SampleInit
     * builds the grid and zeroes its entry count, so registering textures
     * first throws them away. Zeichne renders one block into the grid's memory
     * DC (SampleInit's return value), and the caller blits it into a lane. */
    if (samples && g_gfx && g_screen->hdc) {
        gva AddTex = gfx("GFX_SampleAddTexturePair"), SInit = gfx("GFX_SampleInit");
        /* FONTS' `SAMPLE` block: "Small Fonts", normal, 10. The height is the
         * LAST argument - SampleInit hands CreateFontA argument 6 as the face
         * and argument 7 as nHeight. */
        int griddc = SInit ? (int)X(SInit, 18, 0xa00, (uint32_t)face_a, (uint32_t)face_b, (uint32_t)face_c,
                                    GSTR("Small Fonts"), (uint32_t)fontsize) : 0;
        printf("  GFX_SampleInit              -> %08X (the grid memory DC)\n", griddc);
        int tex[3] = { 0, 0, 0 };
        if (AddTex) {
            /* The palettes are named for the title - DANCE2 for Dance eJay 2,
             * HH2 for HipHop - and without the right ones the blocks register
             * and then draw black. */
            static const char *texa[3] = { "TEXTURE.BMP", "TEXTUREA.BMP", "TEXTUREB.BMP" };
            static const char *texb[3] = { "TEXTURE2.BMP", "TEXTURA2.BMP", "TEXTURB2.BMP" };
            static const char *suffix[3] = { "", "A", "B" };
            for (int i = 0; i < 3; i++) {
                char pal[MAX_PATH];
                snprintf(pal, sizeof(pal), "%s%s.PAL", palname, suffix[i]);
                FILE *pf = xfopen(pal, "rb");           /* Techno eJay ships RAVE2.PAL alone */
                if (pf) fclose(pf);
                else snprintf(pal, sizeof(pal), "%s.PAL", palname);
                tex[i] = (int)X(AddTex, GSTR(texa[i]), GSTR(texb[i]), GSTR(pal), 12, 25, 112);
            }
            printf("  GFX_SampleAddTexturePair    -> %d %d %d\n", tex[0], tex[1], tex[2]);
        }

        /* Composite onto a canvas, so the playback cursor has the arrangement
         * to erase back to, not the empty chrome. */
        HDC wdc = GetDC(wnd);
        g_canvas = CreateCompatibleDC(wdc);
        SelectObject(g_canvas, CreateCompatibleBitmap(wdc, g_screen->w, g_screen->h));
        BitBlt(g_canvas, 0, 0, g_screen->w, g_screen->h, (HDC)(INT_PTR)g_screen->hdc, 0, 0, SRCCOPY);
        g_zeich = gfx("GFX_SampleZeichne"); g_griddc = griddc;
        g_tex[0] = tex[0]; g_tex[1] = tex[1]; g_tex[2] = tex[2];
        draw_blocks(g_canvas);
        printf("  blocks drawn into the grid -> %d\n", g_song_n);
        draw_browser(g_canvas);
        printf("  browser                    -> %d groups, %d samples\n", g_grp_n, g_lib_n);
        BitBlt(wdc, 0, 0, g_screen->w, g_screen->h, g_canvas, 0, 0, SRCCOPY);
        ReleaseDC(wnd, wdc);
    }

    /* ALautSet is 0..32768 on an exponential curve, not a percentage:
     * ALautSet(10) is silence, not a tenth. */
    if (ALautSet) printf("  ALautSet(%d%% = %d/32768) -> %d\n", volume, volume * 32768 / 100,
                         (int)X(ALautSet, (uint32_t)(volume * 32768 / 100)));

    /* The preview path: AFenster(hwnd) -> DStart(0) -> DPlayFile(path, 0,
     * channel), channel = index + 9, all read off Dancejay.exe's call sites. */
    gva DCheck = eng("DPlayFileCheck"), DGetZeit = eng("DGetZeit"), DStart = eng("DStart");
    gva DPlayFile = eng("DPlayFile");
    long total = 0;
    {
        FILE *f = xfopen(playfile, "rb");
        if (f) { fseek(f, 0, SEEK_END); total = ftell(f) - 270; fclose(f); }
    }
    (void)total;
    if (dplay) {
        g_dplayupd = eng("DPlayUpdate");
        if (AFenster)  printf("  AFenster(hwnd)             -> %d\n", (int)X(AFenster, (uint32_t)(uintptr_t)wnd));
        if (DStart)    printf("  DStart(0)                  -> %d\n", (int)X(DStart, 0));
        if (DPlayFile) printf("  DPlayFile(%s, 0, %d) -> %d\n", playfile, chan,
                              (int)X(DPlayFile, GSTR(playfile), 0, (uint32_t)chan));
        if (DCheck)    printf("  DPlayFileCheck(%d)          -> %d\n", chan, (int)X(DCheck, (uint32_t)chan));
    }

    /* ---- the sequencer -------------------------------------------------
     * Dancejay's start-up runs it in this order:
     *
     *     ASetPfad(dir) -> AStop() -> AMitte(0,0) -> RWaveParam(60, 0x6666)
     *       -> ASetFader(0,0) -> APlay(...) per sample
     *
     * and its play button then does ASetFader(0,0) -> AStart(astart), with
     * RTimer pumped every frame. */
    gva AGetTime = eng("AGetTime"), AStart = eng("AStart"), ASetFader = eng("ASetFader");
    g_aplay = eng("APlay");
    if (seq) {
        /* Dancejay builds both of these by concatenating a directory with a
         * name, so the directory carries its own trailing separator and the
         * name APlay is given is absolute. Neither mistake is reported: the
         * sample simply never loads. */
        char dir[MAX_PATH], full[MAX_PATH];
#ifdef _WIN32
        GetCurrentDirectoryA(sizeof(dir), dir);
#else
        {
            static char *b;
            if (!b) b = (char *)gmem(MAX_PATH);
            W(GetCurrentDirectoryA, MAX_PATH, GVA(b));
            snprintf(dir, sizeof(dir), "%s", b);
        }
#endif
        size_t dl = strlen(dir);
        if (dl && dir[dl - 1] != '\\' && dl + 1 < sizeof(dir)) { dir[dl] = '\\'; dir[dl + 1] = 0; }
        if (playfile[0] && playfile[1] == ':') snprintf(full, sizeof(full), "%s", playfile);
        else snprintf(full, sizeof(full), "%s%s", dir, playfile);
        static char *pfad;                      /* the engine may keep the pointer */
        if (!pfad) pfad = (char *)gmem(MAX_PATH);
        snprintf(pfad, MAX_PATH, "%s", dir);

        printf("  ASetPfad(%s)\n", dir);
        X(eng("ASetPfad"), GVA(pfad));
        X0(eng("AStop"));
        X(eng("AMitte"), 0, 0);
        X(eng("RWaveParam"), 0x3c, 0x6666);
        X(ASetFader, 0, 0);
        if (g_aplay && g_lib_n && song) {
            printf("  APlay x%d across %d tracks\n", place_all(ids), g_song_n);
        } else if (g_aplay) {
            /* One sample, placed where --pos says, for timing questions. */
            static SAMPLE *one;
            if (!one) one = (SAMPLE *)gmem(sizeof(SAMPLE));
            snprintf(one->path, sizeof(one->path), "%s", full);
            place(1, 64, one, 0, playpos, playlen ? playlen : BAR_UNITS);
            printf("  APlay(%s) at %d, status %d\n", full, playpos, g_status[64]);
        }
        if (verbose) peek("after APlay");
        if (trackdump) tracks("after APlay", 8);
        /* The intro function's own order: AFenster, then DStart, then AStart. */
        if (AFenster) X(AFenster, (uint32_t)(uintptr_t)wnd);
        if (DStart)   printf("  DStart(0)                  -> %d\n", (int)X(DStart, 0));
        X(ASetFader, 0, 0);
        if (AStart)   printf("  AStart(astart)             -> %d\n", (int)X(AStart, (uint32_t)g_astart));
        if (verbose) peek("after AStart");
        if (trackdump) tracks("after AStart", 8);
    }

    if (selftest && browser_selftest()) return 2;

    float peak = 0.0f;
    static float env[2400];
    static int   envt[2400];   /* AGetTime beside each level: the engine's clock */
    int env_n = 0;
    double played = 0.0;       /* engine bytes from samples that already finished */
    uint32_t t0 = plat_ticks();
    printf("\n  running %d ticks ...\n", ticks);
    /* ATimer is a stub in this engine (`mov eax, 1; ret`); RTimer is the pump.
     * It is what the audio thread calls to arm the mixer when allowed to, and
     * without it on a clock the mixer never arms and DirectSound plays an
     * unfilled buffer, which is loud static. Dancejay pumps it once a frame. */
    gva RTimer = eng("RTimer");
    for (int i = 0; ticks == 0 || i < ticks; i++) {
        if (ticks == 0 && !IsWindow(wnd)) break;
        /* The drag gesture without a mouse: grab the first block, walk it six
         * bars along its lane, drop it; then a browser-to-grid drag, which
         * once crashed. */
        if (dragtest && i == 20 && g_song_n) {
            int bw = bar_width();
            int y = lane_y(g_song[0].lane) + (lane_y(1) - lane_y(0)) / 2;
            int x0 = bar_x(g_song[0].bar) + 4;
            printf("  dragtest: block 0 at bar %d\n", g_song[0].bar);
            browser_input(wnd, WM_LBUTTONDOWN, 0, MAKELPARAM(x0, y));
            for (int b = 1; b <= 6; b++) browser_input(wnd, WM_MOUSEMOVE, 0, MAKELPARAM(x0 + b * bw, y));
            browser_input(wnd, WM_LBUTTONUP, 0, MAKELPARAM(x0 + 6 * bw, y));
            printf("  dragtest: now at bar %d (expected %d)\n", g_song[0].bar, 6);
            int by = (int)g_browse.top + BEZEL_T + 2 * ROW_H + 4;
            int bx = (int)g_browse.left + 40;
            browser_input(wnd, WM_LBUTTONDOWN, 0, MAKELPARAM(bx, by));
            for (int k = 1; k <= 8; k++) browser_input(wnd, WM_MOUSEMOVE, 0, MAKELPARAM(bx + k * 20, by - k * 40));
            browser_input(wnd, WM_LBUTTONUP, 0, MAKELPARAM(GRID_X0 + 200, GRID_Y0 + 40));
            printf("  dragtest: browser-to-grid drag survived\n");
        }
        if (seq && RTimer) X0(RTimer);
        /* The engine only fills its buffer while the mixer is armed; until
         * then what plays is uninitialised noise, so output is held muted. */
        /* Without the flag (PXD32D5), give the mixer a second to arm. */
        if (seq) plat_audio_armed(g_armed ? AT(g_armed, int16_t) > 0 : i > 60);
        if (ATimer) X0(ATimer);
        if (g_dplayupd) X0(g_dplayupd);
        if (main_screen && seq && AGetTime)
            /* AGetTime reports milliseconds. --latency backs the line off by
             * the output delay: measured with --onset, a beat leaves the
             * endpoint 40-100ms after AGetTime says its block started. */
            draw_cursor(wnd, ((int)X(AGetTime, 0) - latency) / (BAR_UNITS / 176.4 * GRID_BARS));
        else if (main_screen && dplay && DGetZeit) {
            /* DGetZeit is a byte offset into the decoded stream. Retrigger the
             * sample when the engine says it has finished. */
            if (DCheck && !X(DCheck, (uint32_t)chan)) {
                played += (int)X(DGetZeit, (uint32_t)chan);
                X(DPlayFile, GSTR(playfile), 0, (uint32_t)chan);
            }
            draw_cursor(wnd, (played + (int)X(DGetZeit, (uint32_t)chan)) / (BAR_UNITS * 16.0));
        }
        int rc = 0;
        /* Refresh's argument is milliseconds into the intro: the DLL compares
         * it against eight thresholds up to 0x7148, so it runs 29 seconds. */
        if (intro && Refresh) rc = (int)X(Refresh, plat_ticks() - t0);
        plat_pump();

        /* A double click in the browser puts that sample into the first lane
         * with room at the bar the cursor is on. The engine takes it live:
         * APlay appends to the track record, and the same slot is drawn. */
        if (g_place >= 0 && g_place < g_lib_n) {
            if (g_song_n < (int)(sizeof(g_song) / sizeof(g_song[0]))) {
                int atbar = (int)(16.0 * g_cursor_frac);
                int lane = 0;
                for (; lane < 16; lane++) {
                    int clash = 0;
                    for (int k = 0; k < g_song_n; k++)
                        if (g_song[k].lane == lane && atbar < g_song[k].bar + g_song[k].bars &&
                            g_song[k].bar < atbar + g_lib[g_place].bars) clash = 1;
                    if (!clash) break;
                }
                if (lane < 16) {
                    SLOT *sl = &g_song[g_song_n++];
                    sl->lane = lane; sl->bar = atbar;
                    sl->bars = g_lib[g_place].bars; sl->lib = g_place;
                    if (g_aplay) place(g_song_n, 65, &g_lib[g_place], lane, atbar * BAR_UNITS, sl->bars * BAR_UNITS);
                    printf("  placed %s / %s -> lane %d, bar %d, %d bars\n",
                           g_lib[g_place].l1, g_lib[g_place].l2, lane, atbar, sl->bars);
                    draw_blocks(g_canvas);
                    HDC dc = GetDC(wnd);
                    BitBlt(dc, GRID_X0, GRID_Y0, GRID_X1 - GRID_X0, GRID_Y1 - GRID_Y0, g_canvas, GRID_X0, GRID_Y0, SRCCOPY);
                    ReleaseDC(wnd, dc);
                }
            }
            g_place = -1;
            g_dirty = 0;
        }
        if (g_dirty && g_canvas) {
            HDC dc = GetDC(wnd);
            draw_browser(g_canvas);
            BitBlt(dc, (int)g_browse.left - 60, (int)g_browse.top - 24,
                   640 - ((int)g_browse.left - 60), (int)g_browse.bottom - (int)g_browse.top + 24,
                   g_canvas, (int)g_browse.left - 60, (int)g_browse.top - 24, SRCCOPY);
            ReleaseDC(wnd, dc);
        }
        g_dirty = 0;
        /* RTimer and the drawing cost about as long as the sleep, so pumping
         * once a frame leaves the window feeling stuck under the mouse. */
        plat_pump();
        float p = plat_peak();
        if (p > peak) peak = p;
        /* Where the playhead is when a beat actually leaves the sound card:
         * a rising edge on the endpoint meter is an onset the user hears. */
        if (onset && main_screen) {
            static int quiet = 8, shots = 0;
            if (p < 0.010f) { if (quiet < 8) quiet++; }
            else if (quiet >= 4) {
                quiet = 0;
                int ms   = AGetTime ? (int)X(AGetTime, 0) : 0;
                double b = ms / (BAR_UNITS / 176.4);
                int headx = bar_x(b), best = -1;
                for (int k = 0; k < g_song_n; k++)
                    if (best < 0 || fabs(g_song[k].bar - b) < fabs(g_song[best].bar - b)) best = k;
                int blockx = best < 0 ? -1 : bar_x(g_song[best].bar);
                printf("    onset at %6d ms = bar %5.2f  playhead x=%3d  nearest block bar %2d x=%3d  off %+4d px %+6.0f ms\n",
                       ms, b, headx, best < 0 ? -1 : g_song[best].bar, blockx, best < 0 ? 0 : headx - blockx,
                       best < 0 ? 0.0 : (b - g_song[best].bar) * (BAR_UNITS / 176.4));
                if (shot && shots < onset) {
                    char f[MAX_PATH];
                    snprintf(f, sizeof(f), "%s.onset%d.bmp", shot, shots++);
                    capture(wnd, 640, 480, f);
                }
            }
        }
        if (env_n < (int)(sizeof(env) / sizeof(env[0]))) {
            envt[env_n] = AGetTime ? (int)X(AGetTime, 0) : 0;
            env[env_n++] = p;
        }
        if (seq && i % 50 == 0)
            printf("    t=%5ums  AGetTime %8d  track 0 at %d of %d  peak %.3f\n",
                   plat_ticks() - t0, AGetTime ? (int)X(AGetTime, 0) : -1,
                   g_tracks ? AT(TRACK(0) + 0x68, int16_t) : -1, g_tracks ? AT(TRACK(0) + 0x6A, int16_t) : -1, peak);
        /* The engine says when it fired an entry: the track's cursor steps on
         * as the transport passes each start. */
        if (trackdump && g_tracks) {
            static short seen[8];
            for (int t = 0; t < 8; t++) {
                gva tr = TRACK(t);
                short cur = AT(tr + 0x68, int16_t);
                if (cur == seen[t]) continue;
                seen[t] = cur;
                gva ents = AT(tr + 0x64, uint32_t);
                if (!ents || cur < 1) continue;
                printf("    fired track %d entry %d start %d at AGetTime %d ms\n",
                       t, cur - 1, *(const int32_t *)HOSTP(ents + (cur - 1) * 0x108),
                       AGetTime ? (int)X(AGetTime, 0) : -1);
            }
        }
        if (verbose && i % 25 == 0)
            printf("    tick %4d  %6ums  refresh %d\n", i, plat_ticks() - t0, rc);
        if (frames && shot && i % frames == 0) {
            char f[MAX_PATH];
            snprintf(f, sizeof(f), "%s.%03d.bmp", shot, i / frames);
            capture(wnd, 640, 480, f);
        }
        plat_sleep(16);
    }
    printf("  endpoint peak during run   -> %.4f\n", peak);
    /* The measured envelope beside the arrangement that was asked for: each
     * row is 128ms, `want` counts the blocks that should be sounding then. A
     * row with blocks and no level is a sample that did not play. */
    if (env_n > 8 && peak > 0.0f) {
        printf("  envelope vs arrangement, one row per 8 ticks (128ms):\n");
        printf("     bar  level  want\n");
        for (int k = 0; k + 8 <= env_n; k += 8) {
            float m = 0;
            for (int j = 0; j < 8; j++) if (env[k + j] > m) m = env[k + j];
            double at = envt[k] * 176.4;
            int want = 0;
            for (int i = 0; i < g_song_n; i++) {
                const SLOT *sl = &g_song[i];
                if (sl->lib < 0 || sl->lib >= g_lib_n) continue;
                double b0 = (double)sl->bar * BAR_UNITS;
                double b1 = b0 + (g_lib[sl->lib].len ? g_lib[sl->lib].len * 2.0 : sl->bars * (double)BAR_UNITS);
                if (at >= b0 && at < b1) want++;
            }
            printf("    %4.1f  %5.3f  %-4d |", at / BAR_UNITS, m, want);
            for (int j = 0; j < (int)(m * 40.0f + 0.5f); j++) putchar('#');
            putchar('\n');
        }
    }
    if (dplay && DCheck) printf("  DPlayFileCheck(%d) at end   -> %d\n", chan, (int)X(DCheck, (uint32_t)chan));
    if (dplay && DGetZeit) printf("  DGetZeit(%d) at end         -> %d\n", chan, (int)X(DGetZeit, (uint32_t)chan));

    int bad = 0;
    if (paintcheck) bad |= paint_check(wnd, shot);
    capture(wnd, 640, 480, shot);
    printf("  done\n");
    return bad ? 3 : 0;
}
