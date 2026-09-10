/* win16_impl.c - the Win16 surface DANCE02.DLL actually uses.
 *
 * DANCE02 imports 60 functions across five modules, and the shape of that list
 * is the whole story of what this DLL is: 27 from KERNEL (global memory, a
 * handful of file calls), 29 from MMSYSTEM (waveOut, waveIn, aux, timeSetEvent),
 * exactly ONE from GDI (BitBlt) and exactly ONE from USER (CallNextHookEx).
 * It is an audio engine with a waveform display bolted to the side.
 *
 * Every shim here obeys one contract, because ne_lift emits
 *     push16(cs); push16(ret_off); MODULE_API(cpu);
 * so on entry SS:SP holds a 4-byte far return address with the PASCAL
 * arguments above it, last-pushed lowest. A shim returns WORD in AX, DWORD in
 * DX:AX, and pops 4 + its purge. Nothing here executes a RETF: the C return is
 * the RETF.
 *
 * The purge values live in tools/win16.py and are echoed into
 * runtime/runtime_api.h as comments, so a mismatch shows up in one diff.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>

#include "cpu.h"
#include "runtime_api.h"

#ifdef EJAY_TRACE_WIN16
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#else
#define LOG(...) ((void)0)
#endif

/* ---- argument access -------------------------------------------------- */
/* off counts bytes up from the last-pushed argument, which sits at sp+4. */
static inline uint16_t a16(CPU *cpu, int off) {
    return mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 4 + off));
}
static inline uint32_t a32(CPU *cpu, int off) {
    return (uint32_t)a16(cpu, off) | ((uint32_t)a16(cpu, off + 2) << 16);
}
static inline void ret(CPU *cpu, int purge) { cpu->sp = (uint16_t)(cpu->sp + 4 + purge); }
static inline void ret16(CPU *cpu, int purge, uint16_t ax) { cpu->ax = ax; ret(cpu, purge); }
static inline void ret32(CPU *cpu, int purge, uint32_t v) {
    cpu->ax = (uint16_t)v; cpu->dx = (uint16_t)(v >> 16); ret(cpu, purge);
}

static void read_asciiz(CPU *cpu, uint16_t seg, uint16_t off, char *out, int max) {
    int i = 0;
    for (; i < max - 1; i++) {
        uint8_t c = mem_read8(cpu, seg, (uint16_t)(off + i));
        if (!c) break;
        out[i] = (char)c;
    }
    out[i] = 0;
}

/* ===== KERNEL: global memory ==========================================
 * Handle == selector, the way Win16 hands back a GMEM_FIXED block. Freed
 * blocks go on a first-fit free list rather than being abandoned: an engine
 * that sizes its sample cache by allocating until failure and then freeing the
 * lot turns a pure bump allocator into a permanent leak.
 * ponytail: first-fit, no splitting or coalescing. Split blocks if a real mix
 * session shows the free list fragmenting. */
static uint32_t g_sel_size[0x10000];
static uint32_t g_sel_base[0x10000];
#define MAX_FREE 4096
static uint32_t fl_base[MAX_FREE], fl_size[MAX_FREE];
static int fl_n;

static uint16_t galloc(CPU *cpu, uint32_t bytes) {
    if (bytes == 0) bytes = 1;
    for (int i = 0; i < fl_n; i++) {            /* reuse a freed block */
        if (fl_size[i] >= bytes) {
            uint16_t sel = cpu->next_sel++;
            if (sel == 0) return 0;
            cpu->sel_base[sel] = fl_base[i];
            for (uint32_t off = 0x10000u; off < fl_size[i]; off += 0x10000u) {
                uint16_t tile = cpu->next_sel++;
                if (tile == 0) break;
                cpu->sel_base[tile] = fl_base[i] + off;
            }
            g_sel_base[sel] = fl_base[i];
            g_sel_size[sel] = fl_size[i];
            fl_base[i] = fl_base[fl_n - 1]; fl_size[i] = fl_size[fl_n - 1]; fl_n--;
            return sel;
        }
    }
    uint32_t base = (cpu->heap_next + 0xFu) & ~0xFu;
    uint16_t sel = cpu_alloc_selector(cpu, bytes);
    if (sel) { g_sel_base[sel] = base; g_sel_size[sel] = bytes; }
    return sel;
}

static void gfree(CPU *cpu, uint16_t sel) {
    (void)cpu;
    if (!sel || !g_sel_size[sel]) return;
    if (fl_n < MAX_FREE) { fl_base[fl_n] = g_sel_base[sel]; fl_size[fl_n] = g_sel_size[sel]; fl_n++; }
    g_sel_size[sel] = 0;
}

void KERNEL_GLOBALALLOC(CPU *cpu) {
    uint16_t flags = a16(cpu, 4);
    uint32_t bytes = a32(cpu, 0);
    uint16_t sel = galloc(cpu, bytes);
    if (sel && (flags & 0x0040))                      /* GMEM_ZEROINIT */
        memset(cpu->mem + g_sel_base[sel], 0, g_sel_size[sel]);
    LOG("[win16] GlobalAlloc(%04X, %u) -> %04X\n", flags, bytes, sel);
    if (sel) cpu->flags &= ~FLAG_CF; else cpu->flags |= FLAG_CF;
    ret16(cpu, 6, sel);
}

void KERNEL_GLOBALREALLOC(CPU *cpu) {
    uint16_t h = a16(cpu, 6);
    uint32_t bytes = a32(cpu, 2);
    if (bytes == 0) bytes = 1;
    uint32_t old = h ? g_sel_size[h] : 0;
    uint16_t out;
    if (bytes <= old) {
        out = h;
    } else {
        out = galloc(cpu, bytes);
        if (out && h) {
            memcpy(cpu->mem + g_sel_base[out], cpu->mem + g_sel_base[h], old);
            gfree(cpu, h);
        }
    }
    if (out) cpu->flags &= ~FLAG_CF; else cpu->flags |= FLAG_CF;
    ret16(cpu, 8, out);
}

void KERNEL_GLOBALFREE(CPU *cpu)    { gfree(cpu, a16(cpu, 0)); ret16(cpu, 2, 0); }
void KERNEL_GLOBALLOCK(CPU *cpu)    { uint16_t h = a16(cpu, 0); cpu->dx = h; ret16(cpu, 2, 0); }
void KERNEL_GLOBALUNLOCK(CPU *cpu)  { ret16(cpu, 2, 0); }
void KERNEL_GLOBALSIZE(CPU *cpu)    { uint16_t h = a16(cpu, 0); ret32(cpu, 2, h ? g_sel_size[h] : 0); }
void KERNEL_GLOBALHANDLE(CPU *cpu)  { uint16_t s = a16(cpu, 0); cpu->dx = s; ret16(cpu, 2, s); }
void KERNEL_LOCKSEGMENT(CPU *cpu)   { ret16(cpu, 2, a16(cpu, 0)); }
void KERNEL_UNLOCKSEGMENT(CPU *cpu) { ret16(cpu, 2, a16(cpu, 0)); }

/* GlobalDosAlloc hands back DX:AX = real-mode paragraph : selector. Nothing
 * here lives below 1 MB, so report the selector in both halves; the engine
 * only ever feeds the result back to GlobalDosFree. */
void KERNEL_GLOBALDOSALLOC(CPU *cpu) {
    uint16_t sel = galloc(cpu, a32(cpu, 0));
    cpu->dx = sel; ret16(cpu, 4, sel);
}
void KERNEL_GLOBALDOSFREE(CPU *cpu) { gfree(cpu, a16(cpu, 0)); ret16(cpu, 2, 0); }

/* GetFreeSpace: what is left of the flat heap, capped at 0x7FFFFFFF so a
 * signed compare on the result cannot go negative. */
void KERNEL_GETFREESPACE(CPU *cpu) {
    uint32_t left = cpu->heap_end - cpu->heap_next;
    ret32(cpu, 2, left > 0x7FFFFFFFu ? 0x7FFFFFFFu : left);
}

/* LocalInit(segment, start, end): the DLL local heap. The lifted code has a
 * real DGROUP to put it in, so report success. */
void KERNEL_LOCALINIT(CPU *cpu) { ret16(cpu, 6, 1); }

/* hmemcpy(dest, src, count) - the huge-pointer copy, so it crosses selectors.
 * Walking it a byte at a time through mem_read8/mem_write8 keeps the tiled
 * selector arithmetic identical to what the guest would have done itself. */
void KERNEL_HMEMCPY(CPU *cpu) {
    uint32_t count = a32(cpu, 0);
    uint16_t src_off = a16(cpu, 4), src_seg = a16(cpu, 6);
    uint16_t dst_off = a16(cpu, 8), dst_seg = a16(cpu, 10);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t s = src_off + i, d = dst_off + i;
        mem_write8(cpu, (uint16_t)(dst_seg + (d >> 16)), (uint16_t)d,
                   mem_read8(cpu, (uint16_t)(src_seg + (s >> 16)), (uint16_t)s));
    }
    ret(cpu, 12);
}

/* ===== KERNEL: files ===================================================
 * HFILE is an index into this table, offset by 1 so 0 stays a valid handle and
 * 0xFFFF stays the error value Win16 callers test for. */
#define MAX_FILES 64
static FILE *g_files[MAX_FILES];
static char g_dir[MAX_PATH];        /* where the game data lives */

void ejay_set_data_dir(const char *dir) {
    strncpy(g_dir, dir ? dir : ".", sizeof(g_dir) - 1);
    g_dir[sizeof(g_dir) - 1] = 0;
}

/* dos.c resolves its find-first patterns against the same root. */
void ejay_data_dir(char *out, int max) {
    snprintf(out, (size_t)max, "%s", g_dir);
}

/* OpenFile(lpszFile, lpOpenBuff, wStyle). Only the styles the engine uses are
 * honoured: OF_READ/OF_WRITE/OF_READWRITE/OF_CREATE, plus OF_EXIST as a probe.
 * A relative name resolves against the data directory, so the engine's own
 * .PXD names work without a chdir. */
void KERNEL_OPENFILE(CPU *cpu) {
    uint16_t style = a16(cpu, 0);
    uint16_t buf_off = a16(cpu, 2), buf_seg = a16(cpu, 4);
    uint16_t name_off = a16(cpu, 6), name_seg = a16(cpu, 8);
    char name[MAX_PATH], path[MAX_PATH * 2];
    read_asciiz(cpu, name_seg, name_off, name, sizeof(name));

    if (name[0] && (name[1] == ':' || name[0] == '\\' || name[0] == '/'))
        snprintf(path, sizeof(path), "%s", name);
    else
        snprintf(path, sizeof(path), "%s\\%s", g_dir[0] ? g_dir : ".", name);

    const char *mode = "rb";
    if (style & 0x1000) mode = "w+b";              /* OF_CREATE */
    else if ((style & 3) == 1) mode = "r+b";       /* OF_WRITE */
    else if ((style & 3) == 2) mode = "r+b";       /* OF_READWRITE */

    if (style & 0x4000) {                          /* OF_EXIST: probe only */
        FILE *probe = fopen(path, "rb");
        if (probe) fclose(probe);
        LOG("[win16] OpenFile(EXIST %s) -> %s\n", path, probe ? "yes" : "no");
        ret16(cpu, 10, probe ? 1 : 0xFFFF);
        return;
    }

    int slot = -1;
    for (int i = 0; i < MAX_FILES; i++) if (!g_files[i]) { slot = i; break; }
    FILE *f = (slot >= 0) ? fopen(path, mode) : NULL;
    LOG("[win16] OpenFile(%s, style=%04X) -> %s\n", path, style, f ? "ok" : "FAIL");
    if (!f) { ret16(cpu, 10, 0xFFFF); return; }
    g_files[slot] = f;

    /* OFSTRUCT: cBytes, fFixedDisk, nErrCode, reserved[4], szPathName[128] */
    if (buf_seg) {
        int len = (int)strlen(path);
        mem_write8(cpu, buf_seg, buf_off, (uint8_t)136);
        mem_write8(cpu, buf_seg, (uint16_t)(buf_off + 1), 1);
        mem_write16(cpu, buf_seg, (uint16_t)(buf_off + 2), 0);
        for (int i = 0; i < 128; i++)
            mem_write8(cpu, buf_seg, (uint16_t)(buf_off + 8 + i),
                       (uint8_t)(i < len ? path[i] : 0));
    }
    ret16(cpu, 10, (uint16_t)(slot + 1));
}

static FILE *hfile(uint16_t h) {
    return (h && h != 0xFFFF && h <= MAX_FILES) ? g_files[h - 1] : NULL;
}

/* The engine opens a sample with OpenFile and then seeks in it with a raw
 * INT 21h, so dos.c has to reach the same table: one handle space, two APIs. */
FILE *ejay_hfile(uint16_t h) { return hfile(h); }

void KERNEL__LCLOSE(CPU *cpu) {
    uint16_t h = a16(cpu, 0);
    FILE *f = hfile(h);
    if (f) { fclose(f); g_files[h - 1] = NULL; }
    ret16(cpu, 2, f ? 0 : 0xFFFF);
}

/* _hread(hf, hpvBuffer, cbRead) - the huge-pointer read, so a sample bigger
 * than 64 KB lands across tiled selectors instead of wrapping at 0xFFFF. */
void KERNEL__HREAD(CPU *cpu) {
    uint32_t count = a32(cpu, 0);
    uint16_t off = a16(cpu, 4), seg = a16(cpu, 6);
    FILE *f = hfile(a16(cpu, 8));
    if (!f) { ret32(cpu, 10, 0xFFFFFFFFu); return; }
    uint32_t done = 0;
    uint8_t chunk[4096];
    while (done < count) {
        size_t want = (count - done < sizeof(chunk)) ? (size_t)(count - done) : sizeof(chunk);
        size_t got = fread(chunk, 1, want, f);
        for (size_t i = 0; i < got; i++) {
            uint32_t d = off + done + (uint32_t)i;
            mem_write8(cpu, (uint16_t)(seg + (d >> 16)), (uint16_t)d, chunk[i]);
        }
        done += (uint32_t)got;
        if (got < want) break;
    }
    LOG("[win16] _hread -> %u of %u\n", done, count);
    ret32(cpu, 10, done);
}

void KERNEL__HWRITE(CPU *cpu) {
    uint32_t count = a32(cpu, 0);
    uint16_t off = a16(cpu, 4), seg = a16(cpu, 6);
    FILE *f = hfile(a16(cpu, 8));
    if (!f) { ret32(cpu, 10, 0xFFFFFFFFu); return; }
    uint32_t done = 0;
    uint8_t chunk[4096];
    while (done < count) {
        uint32_t want = (count - done < sizeof(chunk)) ? (count - done) : (uint32_t)sizeof(chunk);
        for (uint32_t i = 0; i < want; i++) {
            uint32_t s = off + done + i;
            chunk[i] = mem_read8(cpu, (uint16_t)(seg + (s >> 16)), (uint16_t)s);
        }
        uint32_t put = (uint32_t)fwrite(chunk, 1, want, f);
        done += put;
        if (put < want) break;
    }
    ret32(cpu, 10, done);
}

void KERNEL_GETTEMPFILENAME(CPU *cpu) {
    uint16_t out_off = a16(cpu, 0), out_seg = a16(cpu, 2);
    uint16_t unique = a16(cpu, 4);
    uint16_t pre_off = a16(cpu, 6), pre_seg = a16(cpu, 8);
    char pre[8];
    read_asciiz(cpu, pre_seg, pre_off, pre, sizeof(pre));
    if (!unique) unique = (uint16_t)(GetTickCount() & 0x7FFF);
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\%.3s%04X.TMP", g_dir[0] ? g_dir : ".", pre, unique);
    for (int i = 0; i <= (int)strlen(path); i++)
        mem_write8(cpu, out_seg, (uint16_t)(out_off + i), (uint8_t)path[i]);
    ret16(cpu, 12, unique);
}

void KERNEL_GETDRIVETYPE(CPU *cpu) { ret16(cpu, 2, 3); }   /* DRIVE_FIXED */

/* GetDOSEnvironment returns a far pointer to a double-NUL environment block.
 * Callers walk it rather than testing it, so hand back the guard region, which
 * is already zeroed - an empty environment, not a null pointer. */
void KERNEL_GETDOSENVIRONMENT(CPU *cpu) { cpu->dx = 0; ret16(cpu, 0, 0); }

/* ===== KERNEL: system ================================================== */
void KERNEL_GETVERSION(CPU *cpu)     { ret16(cpu, 0, 0x0A03); }  /* DOS 10, Win 3.10 */
void KERNEL_GETWINFLAGS(CPU *cpu)    { ret32(cpu, 0, 0x0403); }  /* 386 enhanced + 80387 */
void KERNEL_INITTASK(CPU *cpu)       { ret16(cpu, 0, 1); }
void KERNEL_GETMODULEUSAGE(CPU *cpu) { ret16(cpu, 2, 1); }
void KERNEL_FATALEXIT(CPU *cpu) {
    fprintf(stderr, "[win16] FatalExit(%d) - the DLL gave up\n", (int)(int16_t)a16(cpu, 0));
    exit(1);
}
void KERNEL_FATALAPPEXIT(CPU *cpu) {
    char msg[256];
    read_asciiz(cpu, a16(cpu, 2), a16(cpu, 0), msg, sizeof(msg));
    fprintf(stderr, "[win16] FatalAppExit: %s\n", msg);
    exit(1);
}

/* ===== MMSYSTEM: the clock ============================================= */
void MMSYSTEM_TIMEGETTIME(CPU *cpu)     { ret32(cpu, 0, timeGetTime()); }
void MMSYSTEM_TIMEBEGINPERIOD(CPU *cpu) { ret16(cpu, 2, (uint16_t)timeBeginPeriod(a16(cpu, 0))); }
void MMSYSTEM_TIMEENDPERIOD(CPU *cpu)   { ret16(cpu, 2, (uint16_t)timeEndPeriod(a16(cpu, 0))); }

/* ===== MMSYSTEM: device counts ========================================= */
void MMSYSTEM_WAVEOUTGETNUMDEVS(CPU *cpu) { ret16(cpu, 0, (uint16_t)waveOutGetNumDevs()); }
void MMSYSTEM_AUXGETNUMDEVS(CPU *cpu)     { ret16(cpu, 0, (uint16_t)auxGetNumDevs()); }

/* ===== the two odd ones out ============================================
 * One GDI call and one USER call in the whole DLL. BitBlt is the waveform
 * display; CallNextHookEx is AHook, the engine's message hook. Neither is on
 * the audio path, so both are honest no-ops until there is a window to draw
 * into. */
/* GDI_BITBLT lives in video.c: it has a window to draw into. */
void USER_CALLNEXTHOOKEX(CPU *cpu) { ret32(cpu, 10, 0); }

/* WIN87EM.__FPMATH is the floating-point emulator entry. The lifted code has
 * real x87 in its instruction stream and cpu.h models it directly, so nothing
 * should route here; say so if it does. */
void WIN87EM___FPMATH(CPU *cpu) {
    (void)cpu;
    static int once;
    if (!once++) fprintf(stderr, "[win16] __FPMATH reached - x87 emulation path taken\n");
}
