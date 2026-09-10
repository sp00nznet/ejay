/* main.c - harness for the recompiled 1997 Dance eJay audio engine.
 *
 * DANCE02.DLL is a Win16 DLL, so it has no entry point of its own to run: it
 * expects a host to load it, call LibMain, and then call its exports. In 1997
 * that host was DANCE.EXE, 212 KB of Visual Basic 4 p-code that cannot be
 * lifted. This stands in for it - it builds the flat memory image, gives the
 * DLL the stack a DLL never brings with it, runs LibMain, and then calls
 * whichever export you name on the command line.
 *
 *     ejay                       run LibMain and list the exports
 *     ejay AInit                 run LibMain, then call AInit
 *     ejay --dir <path> AInit    ... with that directory as the data root
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>

#include "cpu.h"
#include "segments.h"
#include "runtime_api.h"
#include "exports.h"

#ifndef EJAY_IMAGE_PATH
#define EJAY_IMAGE_PATH "build_data/mem_image.bin"
#endif

/* cpu.h's tracing hooks. The ring buffer costs one store per lifted call and
 * turns a host crash into a readable guest backtrace, which is the only
 * backtrace there is - the guest call stack lives in cpu->mem. */
const char *g_fn_ring[EJAY_FN_RING_SIZE];
unsigned g_fn_ring_pos;
int g_fncount;
int g_watch_ds;
const char *g_ds_zero_from;
unsigned g_ds_run;
uint16_t g_watch_seg, g_watch_off;
int g_watch_armed;
uint16_t g_wsel;

static CPU *g_cpu;

static void guest_backtrace(void);

/* Function watch. cpu.h calls this from TRACE_FN on every lifted call when
 * g_fncount is set, so --watch turns "does this function ever run" from a
 * call-graph guess into a number. The static graph cannot follow
 * dispatch_far, so it has already been wrong twice about exactly that.
 * ponytail: linear strcmp over a handful of names. Fine at 8 watches. */
#define MAX_WATCH 8
static const char *w_name[MAX_WATCH];
static unsigned long w_hits[MAX_WATCH];
static int w_n;

void ejay_fn_hit(const char *n)
{
    for (int i = 0; i < w_n; i++)
        if (strcmp(n, w_name[i]) == 0) { w_hits[i]++; return; }
}

/* Selector-write watchpoint. g_wsel is 0 unless something sets it, so this
 * never fires in a normal run; it exists because "which lifted function
 * scribbled on that selector" is otherwise unanswerable. */
/* Who writes to a watched selector. The mix buffer takes hundreds of
 * thousands of writes when it works, so these are counted per lifted
 * function rather than printed - the question is only whether ANYTHING
 * writes it, and from where. */
#define SELW_SLOTS 12
static const char *selw_who[SELW_SLOTS];
static unsigned long selw_hits[SELW_SLOTS];
unsigned long g_selw_total;
static unsigned long *selw_bysel;

void ejay_sel_write(uint16_t seg, uint16_t off, uint16_t val)
{
    const char *fn = g_fn_ring[(g_fn_ring_pos - 1u) & (EJAY_FN_RING_SIZE - 1)];
    (void)off; (void)val;
    g_selw_total++;
    if (g_wsel == 0xFFFFu) {
        /* Watching everything: count by SELECTOR, so the block that receives
         * decoded audio shows up without having to guess which one it is. */
        if (!selw_bysel) selw_bysel = (unsigned long *)calloc(65536, sizeof(unsigned long));
        if (selw_bysel) selw_bysel[seg]++;
        return;
    }
    for (int k = 0; k < SELW_SLOTS; k++) {
        if (!selw_who[k]) { selw_who[k] = fn; selw_hits[k] = 1; return; }
        if (selw_who[k] == fn) { selw_hits[k]++; return; }
    }
}

static void selw_report(void)
{
    printf("  %lu guest writes seen\n", g_selw_total);
    if (selw_bysel) {
        for (int top = 0; top < 10; top++) {
            unsigned best = 0; unsigned long bn = 0;
            for (unsigned k = 1; k < 65536; k++)
                if (selw_bysel[k] > bn) { bn = selw_bysel[k]; best = k; }
            if (!bn) break;
            printf("    sel %04X  %lu writes\n", best, bn);
            selw_bysel[best] = 0;
        }
        return;
    }
    for (int k = 0; k < SELW_SLOTS && selw_who[k]; k++)
        printf("    %-16s %lu\n", selw_who[k], selw_hits[k]);
}

/* Guest divide by zero. On real hardware this is INT 0; here the registers are
 * left alone and the run continues, because a lifter that mis-sized one
 * operand produces a stream of these and the first few say where. */
void ejay_div0(const char *kind)
{
    static int fired;
    if (fired++ >= 6) return;
    fprintf(stderr, "[div0] guest %s by zero; registers unchanged\n", kind);
    if (fired == 1) guest_backtrace();
}

void ejay_set_data_dir(const char *dir);
void ejay_set_test_volume(int pct);   /* -1 leaves the engine in charge */
int  ejay_video_open(int w, int h);
void ejay_video_pump(void);
void ejay_video_close(void);
int  ejay_video_blits(void);
int  ejay_video_save(const char *path);
int  ejay_video_background(const char *path);
void ejay_video_restore(void);
void ejay_wave_dump(const char *path);
void ejay_wave_dump_close(void);

/* The multimedia timer, from runtime/win16/wave.c. The engine asks for a 32 ms
 * tick whose callback is DanceTimer, and a real timer would deliver it on the
 * driver's thread - into a CPU model that is one struct with one stack. So the
 * timer is registered but not started, and the tick is run here instead, on
 * the thread that owns the CPU. */
int  ejay_timer_pending(void);
void ejay_timer_fire(CPU *cpu, void (*dispatch)(CPU *, uint16_t, uint16_t));

/* ---- the indirect-call dispatchers live in src/_dispatch.c ---- */

void int_handler(CPU *cpu, int int_num) {
    (void)cpu;
    fprintf(stderr, "[int] INT %02Xh from lifted code - unhandled\n", int_num);
}
void dos_int21(CPU *cpu) { int_handler(cpu, 0x21); }

static void guest_backtrace(void) {
    fprintf(stderr, "guest backtrace (most recent first):\n");
    for (int i = 1; i <= 24; i++) {
        const char *nm = g_fn_ring[(g_fn_ring_pos - (unsigned)i) & (EJAY_FN_RING_SIZE - 1)];
        if (!nm) break;
        fprintf(stderr, "  %s\n", nm);
    }
}

static LONG WINAPI crash_handler(EXCEPTION_POINTERS *ep) {
    fprintf(stderr, "\n*** exception %08lX at %p ***\n",
            ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    if (g_cpu)
        fprintf(stderr, "cs=%04X ip=?  ds=%04X es=%04X ss=%04X sp=%04X  "
                        "ax=%04X bx=%04X cx=%04X dx=%04X\n",
                g_cpu->cs, g_cpu->ds, g_cpu->es, g_cpu->ss, g_cpu->sp,
                g_cpu->ax, g_cpu->bx, g_cpu->cx, g_cpu->dx);
    guest_backtrace();
    return EXCEPTION_EXECUTE_HANDLER;
}

static int load_image(CPU *cpu, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open image: %s\n", path); return 0; }
    size_t n = fread(cpu->mem, 1, EJAY_IMAGE_SIZE, f);
    fclose(f);
    if (n != EJAY_IMAGE_SIZE) {
        fprintf(stderr, "short read: %zu of %u\n", n, (unsigned)EJAY_IMAGE_SIZE);
        return 0;
    }
    return 1;
}

static const EjayExport *find_export(const char *name) {
    for (int i = 0; i < EJAY_NUM_EXPORTS; i++)
        if (_stricmp(EJAY_EXPORTS[i].name, name) == 0)
            return &EJAY_EXPORTS[i];
    return NULL;
}

static void list_exports(void) {
    printf("\n%d exports, with the names their author gave them:\n", EJAY_NUM_EXPORTS);
    for (int i = 0; i < EJAY_NUM_EXPORTS; i++) {
        printf("  @%-3d %-16s%s", EJAY_EXPORTS[i].ordinal, EJAY_EXPORTS[i].name,
               EJAY_EXPORTS[i].fn ? "" : " (not lifted)");
        if ((i % 3) == 2) printf("\n");
    }
    if ((EJAY_NUM_EXPORTS % 3) != 0) printf("\n");
}

/* Report the non-zero bytes in a DGROUP range. The engine keeps its tables
 * sparse - 1600 slots with a handful marked - so a hex dump is mostly noise;
 * what matters is WHICH offsets are set. */
static void dump_range(CPU *cpu, uint16_t off, uint16_t len)
{
    unsigned shown = 0, total = 0;
    printf("ds:[%04X..%04X]:\n", off, (uint16_t)(off + len - 1));
    for (uint16_t i = 0; i < len; i++) {
        uint8_t v = mem_read8(cpu, EJAY_AUTO_DATA_SEG, (uint16_t)(off + i));
        if (!v) continue;
        total++;
        if (shown < 32) {
            printf("  +%04X = %02X\n", i, v);
            shown++;
        }
    }
    printf("  %u non-zero bytes in %u\n", total, len);
}

/* Summarise a range in ANY selector as 16-bit PCM: how much of it is
 * non-zero and how loud. The engine mixes into GlobalAlloc blocks whose
 * selectors are handed out at runtime, so "is there audio in there" needs
 * to be answerable for an arbitrary selector, not just DGROUP. */
static void pcm_summary(CPU *cpu, uint16_t sel, uint32_t len)
{
    uint32_t nz = 0, n = len / 2;
    int32_t peak = 0;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t off = k * 2;
        int32_t v = (int16_t)mem_read16(cpu, (uint16_t)(sel + (off >> 16)),
                                        (uint16_t)off);
        if (v) nz++;
        if (v < 0) v = -v;
        if (v > peak) peak = v;
    }
    printf("  sel %04X: %u/%u non-zero, peak %d\n", sel, nz, n, (int)peak);
}

/* Scan the whole flat image for the .PXD magic. The loader computes where
 * to put the file through huge-pointer arithmetic, and the parser computes
 * where to read it the same way; if those two disagree the file is simply
 * not where anyone looks. Reporting the flat offset settles it. */
static void find_magic(CPU *cpu)
{
    static const uint8_t MAGIC[4] = { 0x74, 0x50, 0x78, 0x44 };   /* tPxD */
    unsigned hits = 0;
    for (uint32_t a = 0; a + 4 <= cpu->mem_size; a++) {
        if (memcmp(cpu->mem + a, MAGIC, 4) != 0) continue;
        printf("  tPxD at flat %u (0x%X)\n", a, a);
        if (++hits >= 8) break;
    }
    if (!hits) printf("  tPxD not present anywhere in guest memory\n");
}

/* Set the guest up as if a Win16 host were about to make a far call into the
 * DLL: fresh stack, DGROUP in DS, and the arguments below a far return address
 * so the callee's RETF has somewhere to land.
 *
 * Win16 PASCAL pushes arguments LEFT TO RIGHT, so the first argument ends up
 * deepest and the last sits just above the return address. A DWORD goes
 * high word first; a far pointer goes segment first. Both leave the low half
 * at the lower address, which is what the shims read back. */
static void enter_guest(CPU *cpu) {
    cpu->ds = cpu->es = EJAY_AUTO_DATA_SEG;
    cpu->ss = EJAY_STACK_SEG;
    cpu->sp = EJAY_STACK_SP;
    cpu->cs = EJAY_CODE_SEG;
}

static void push_retaddr(CPU *cpu) {
    push16(cpu, 0);            /* return CS: nothing to return to */
    push16(cpu, 0);            /* return IP */
}

/* Scratch guest memory for string arguments, handed out of the stack segment
 * well below SP so a call cannot walk over it. */
static uint16_t g_scratch = 0x0100;

static void push_string(CPU *cpu, const char *text) {
    uint16_t at = g_scratch;
    for (size_t i = 0; ; i++) {
        mem_write8(cpu, EJAY_STACK_SEG, (uint16_t)(at + i), (uint8_t)text[i]);
        if (!text[i]) break;
    }
    g_scratch = (uint16_t)(at + strlen(text) + 1);
    push16(cpu, EJAY_STACK_SEG);       /* segment first ... */
    push16(cpu, at);                   /* ... then offset */
}

/* One argument token. Returns bytes pushed, or -1 on a token we cannot read.
 *   123 / 0x7B   a WORD
 *   d:123        a DWORD (high word first)
 *   s:TEXT       a far pointer to TEXT, placed in guest memory */
static int push_arg(CPU *cpu, const char *tok) {
    if (!strncmp(tok, "s:", 2)) { push_string(cpu, tok + 2); return 4; }
    if (!strncmp(tok, "d:", 2)) {
        unsigned long v = strtoul(tok + 2, NULL, 0);
        push16(cpu, (uint16_t)(v >> 16));
        push16(cpu, (uint16_t)v);
        return 4;
    }
    char *end = NULL;
    unsigned long v = strtoul(tok, &end, 0);
    if (end == tok || (end && *end)) return -1;
    push16(cpu, (uint16_t)v);
    return 2;
}

/* "AInit" or "AInit:1,s:BINP.PXD". Pushes the arguments, then the return
 * address. Returns bytes of arguments pushed, or -1. */
static int push_args(CPU *cpu, char *spec) {
    char *colon = strchr(spec, ':');
    int bytes = 0;
    if (colon && (colon[1] == '\0')) colon = NULL;
    /* s: and d: contain colons of their own, so split on the FIRST one only */
    if (colon) {
        *colon = '\0';
        char *p = colon + 1;
        while (*p) {
            char *comma = strchr(p, ',');
            if (comma) *comma = '\0';
            int n = push_arg(cpu, p);
            if (n < 0) { fprintf(stderr, "bad argument: %s\n", p); return -1; }
            bytes += n;
            if (!comma) break;
            p = comma + 1;
        }
    }
    push_retaddr(cpu);
    return bytes;
}

/* Feed a catalogue file to ASortIn, one path per line. MIN.TXT and
 * MAX.TXT on the disc are exactly that: 896 and 1,351 quoted sample
 * paths in the `ba\\aaaf.pxd` form the engine builds for itself. The VB
 * host read one of them at startup and handed the engine its library,
 * which is the step no amount of probing from outside was going to
 * guess. */
static int feed_sortlist(CPU *cpu, const EjayExport *sortin, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 0; }
    char line[512];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        char clean[512]; int c = 0;
        for (char *q = line; *q && c < (int)sizeof(clean) - 1; q++)
            if (*q != '"' && *q != '\r' && *q != '\n') clean[c++] = *q;
        clean[c] = 0;
        if (!c) continue;
        enter_guest(cpu);
        push_string(cpu, clean);
        push_retaddr(cpu);
        sortin->fn(cpu);
        n++;
    }
    fclose(f);
    printf("  fed %d sample paths to ASortIn\n", n);
    return n;
}

int main(int argc, char **argv) {
    const char *img = EJAY_IMAGE_PATH;
    const char *dir = ".";
    char *call[16];
    int ncall = 0;
    int ring = 0;
    /* DGROUP offsets to report after each call. The engine says almost nothing
     * through its return values - AInit answers with a bitmask in ds:[0xA4] -
     * so watching its own state is the only way to see what a call achieved. */
    uint16_t peek[8];
    int npeek = 0;
    uint16_t dump_off = 0, dump_len = 0;
    int pump_ms = 0;
    int volume = 20;               /* percent; bring-up runs stay quiet */
    int magic = 0;
    int window = 0;
    const char *shot = NULL;
    const char *wav = NULL;
    const char *sortlist = NULL;
    const char *bg = NULL;
    int hot = 0;
    int unstick = 0;
    unsigned wsel = 0;
    uint16_t pcm_sel[8]; uint32_t pcm_len = 0; int npcm = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--image") && i + 1 < argc) img = argv[++i];
        else if (!strcmp(argv[i], "--ring")) ring = 40;
        else if (!strcmp(argv[i], "--peek") && i + 1 < argc && npeek < 8)
            peek[npeek++] = (uint16_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--dump") && i + 2 < argc) {
            dump_off = (uint16_t)strtoul(argv[++i], NULL, 0);
            dump_len = (uint16_t)strtoul(argv[++i], NULL, 0);
        }
        else if (!strcmp(argv[i], "--pcm") && i + 2 < argc && npcm < 8) {
            pcm_sel[npcm++] = (uint16_t)strtoul(argv[++i], NULL, 0);
            pcm_len = strtoul(argv[++i], NULL, 0);
        }
        else if (!strcmp(argv[i], "--wsel") && i + 1 < argc)
            wsel = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) { shot = argv[++i]; window = 1; }
        else if (!strcmp(argv[i], "--unstick")) unstick = 1;
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav = argv[++i];
        else if (!strcmp(argv[i], "--sortlist") && i + 1 < argc) sortlist = argv[++i];
        else if (!strcmp(argv[i], "--bg") && i + 1 < argc) { bg = argv[++i]; window = 1; }
        else if (!strcmp(argv[i], "--hot")) hot = 1;
        else if (!strcmp(argv[i], "--window")) window = 1;
        else if (!strcmp(argv[i], "--magic")) magic = 1;
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc && w_n < MAX_WATCH) {
            w_name[w_n++] = argv[++i];
            g_fncount = 1;            /* arms TRACE_FN's callback */
        }
        else if (!strcmp(argv[i], "--volume") && i + 1 < argc)
            volume = (int)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--pump") && i + 1 < argc)
            pump_ms = (int)strtoul(argv[++i], NULL, 0);
        else if (ncall < 16) call[ncall++] = argv[i];
    }

    SetUnhandledExceptionFilter(crash_handler);
    ejay_set_data_dir(dir);
    ejay_set_test_volume(volume);
    if (wav) ejay_wave_dump(wav);
    g_wsel = (uint16_t)wsel;      /* arms cpu.h EJAY_SELW on guest writes */
    if (window && !ejay_video_open(640, 480))
        fprintf(stderr, "could not open a window; drawing goes nowhere\n");
    if (bg) ejay_video_background(bg);

    CPU cpu;
    cpu_init(&cpu);
    g_cpu = &cpu;

    /* The image plus a heap for GlobalAlloc. 16 MB is period-appropriate for a
     * 1997 sampler and, more usefully, bounds the engine's allocate-until-fail
     * memory probe to a count a 1997 machine would have reported. */
    uint32_t total = EJAY_IMAGE_SIZE + (16u << 20);
    if (!cpu_alloc_mem(&cpu, total)) {
        fprintf(stderr, "cannot allocate %u bytes\n", total);
        return 1;
    }
    if (!load_image(&cpu, img)) { cpu_free(&cpu); return 1; }

    printf("Dance eJay 1997 audio engine - recompiled\n");
    printf("  image      %s (%u bytes, %u segments + stack)\n",
           img, (unsigned)EJAY_IMAGE_SIZE, EJAY_NUM_SEG - 1);
    printf("  LibMain    seg%u:%04X\n", EJAY_ENTRY_SEG, EJAY_ENTRY_IP);
    printf("  DGROUP     seg%u      stack seg%u:%04X\n",
           EJAY_AUTO_DATA_SEG, EJAY_STACK_SEG, EJAY_STACK_SP);
    printf("  data dir   %s\n", dir);
    fflush(stdout);

    /* Win16 DLL entry contract: DI = hInstance, DS = DGROUP, CX = the heap
     * size out of the NE header, ES:SI = command line. CX matters - a DLL that
     * sees CX == 0 takes its jcxz path and skips LocalInit entirely, and then
     * runs with no local heap at all. */
    enter_guest(&cpu);
    push_retaddr(&cpu);
    cpu.di = EJAY_AUTO_DATA_SEG;
    cpu.cx = EJAY_HEAP_SIZE;
    cpu.si = 0;
    seg001_0054(&cpu);
    printf("\nLibMain returned ax=%04X (%s)\n", cpu.ax,
           cpu.ax ? "success" : "FAILED - the DLL refused to initialise");

    if (!ncall) {
        list_exports();
        cpu_free(&cpu);
        return 0;
    }

    /* Each named export is a fresh far call into a DLL that is already
     * initialised: the stack restarts, the engine's state in DGROUP does not.
     * That is exactly how DANCE.EXE drove it, one Declare at a time. */
    for (int i = 0; i < ncall; i++) {
        char spec[256];
        snprintf(spec, sizeof(spec), "%s", call[i]);
        char name[64];
        snprintf(name, sizeof(name), "%s", spec);
        char *colon = strchr(name, ':');
        if (colon) *colon = '\0';

        /* `sortlist` in the call sequence feeds the catalogue at that point,
         * so the library can be loaded before whatever comes next needs it. */
        if (!strcmp(name, "sortlist")) {
            const EjayExport *si = find_export("ASortIn");
            if (si && si->fn && sortlist) { g_scratch = 0x0100; feed_sortlist(&cpu, si, sortlist); }
            else fprintf(stderr, "sortlist named in the sequence but --sortlist not given\n");
            continue;
        }

        const EjayExport *e = find_export(name);
        if (!e) {
            fprintf(stderr, "no such export: %s\n", name);
            list_exports();
            cpu_free(&cpu);
            return 1;
        }
        if (!e->fn) {
            fprintf(stderr, "%s has no lifted function: its entry point did not "
                            "land on an instruction boundary\n", e->name);
            cpu_free(&cpu);
            return 1;
        }

        enter_guest(&cpu);
        int bytes = push_args(&cpu, spec);
        if (bytes < 0) { cpu_free(&cpu); return 1; }
        printf("\ncalling %s (@%d) with %d bytes of arguments ...\n",
               e->name, e->ordinal, bytes);
        fflush(stdout);

        uint16_t sp_before = cpu.sp;
        unsigned ring_at = g_fn_ring_pos;
        e->fn(&cpu);
        if (ring) {
            /* The lifted functions this call actually went through. There is
             * no other backtrace: the guest call stack lives in cpu->mem. */
            unsigned n = g_fn_ring_pos - ring_at;
            printf("  %u lifted calls; last %d:\n", n, ring < (int)n ? ring : (int)n);
            for (int k = (ring < (int)n ? ring : (int)n); k >= 1; k--)
                printf("    %s\n",
                       g_fn_ring[(g_fn_ring_pos - (unsigned)k) & (EJAY_FN_RING_SIZE - 1)]);
        }
        for (int k = 0; k < npeek; k++)
            printf("  ds:[%04X] = %02X  %04X  %08X\n", peek[k],
                   mem_read8(&cpu, EJAY_AUTO_DATA_SEG, peek[k]),
                   mem_read16(&cpu, EJAY_AUTO_DATA_SEG, peek[k]),
                   mem_read32(&cpu, EJAY_AUTO_DATA_SEG, peek[k]));
        if (dump_len) dump_range(&cpu, dump_off, dump_len);
        printf("%s returned ax=%04X dx=%04X", e->name, cpu.ax, cpu.dx);
        /* A PASCAL callee pops the return address and its own arguments, so SP
         * must come back exactly 4 + bytes higher. Anything else means a purge
         * somewhere was wrong, and in a real host the caller's frame would
         * have been the next casualty. */
        uint16_t expect = (uint16_t)(sp_before + 4 + bytes);
        if (cpu.sp != expect)
            printf("   *** sp %04X, expected %04X (off by %d) ***",
                   cpu.sp, expect, (int)(int16_t)(cpu.sp - expect));
        printf("\n");
    }

    if (sortlist) {
        const EjayExport *si = find_export("ASortIn");
        if (si && si->fn) {
            /* g_scratch would run off the end of the stack segment over
             * hundreds of paths, so it is rewound for each one - the engine
             * copies the string out before returning. */
            g_scratch = 0x0100;
            feed_sortlist(&cpu, si, sortlist);
        }
    }

    /* Run the engine's own clock. DanceTimer is what refills and queues the
     * mixing buffers, so without it the engine is initialised and silent. Each
     * tick is a far call into lifted code from the thread that owns the CPU,
     * which is why the timer was never handed to the driver in the first
     * place. */
    if (pump_ms > 0) {
        printf("\npumping the engine's 32 ms clock for %d ms ...\n", pump_ms);
        fflush(stdout);
        DWORD until = GetTickCount() + (DWORD)pump_ms;
        unsigned ticks = 0;
        unsigned before = g_fn_ring_pos;
        const EjayExport *atimer = find_export("ATimer");
        while ((int32_t)(GetTickCount() - until) < 0) {
            if (ejay_timer_pending()) {
                enter_guest(&cpu);
                ejay_timer_fire(&cpu, dispatch_far);
                /* ATimer is the other half of the clock. DanceTimer is what
                 * timeSetEvent registers, but the mixer's waveOut path hangs
                 * off ATimer - which the VB front end called from a Timer
                 * control of its own. Both have to run, or the engine keeps
                 * time and never queues a buffer. */
                if (atimer && atimer->fn) {
                    ejay_video_restore();     /* the host owns the background */
                    enter_guest(&cpu);
                    push_retaddr(&cpu);
                    atimer->fn(&cpu);
                }
                /* Diagnostic, not a fix. The loader parks a voice at 0xFFFE
                 * after reading its file and 4ED4 will only revisit a voice
                 * whose flag is 0, so the parse pass never runs. Forcing the
                 * flag back tests whether that transition is the only thing
                 * between here and sound. */
                if (unstick) {
                    for (int v = 0; v < 8; v++) {
                        uint16_t rec = (uint16_t)(0x8BC + v * 0x102 + 0x44);
                        if (mem_read16(&cpu, EJAY_AUTO_DATA_SEG, rec) == 0xFFFE)
                            mem_write16(&cpu, EJAY_AUTO_DATA_SEG, rec, 0);
                    }
                }
                ticks++;
            } else if (hot && atimer && atimer->fn) {
                /* The engine mixes on demand, and in 1997 the host called
                 * ATimer from a VB Timer far more often than the 32 ms
                 * sequencer tick. --hot drives it as fast as it will go. */
                enter_guest(&cpu);
                push_retaddr(&cpu);
                atimer->fn(&cpu);
            } else {
                ejay_video_pump();
                Sleep(1);
            }
        }
        printf("%u ticks, %u lifted calls\n", ticks, g_fn_ring_pos - before);
        if (wsel) selw_report();
        if (window) printf("  %d BitBlt calls\n", ejay_video_blits());
        for (int k = 0; k < w_n; k++)
            printf("  %s ran %lu times\n", w_name[k], w_hits[k]);
        if (magic) find_magic(&cpu);
        if (dump_len) dump_range(&cpu, dump_off, dump_len);
        for (int k = 0; k < npcm; k++) pcm_summary(&cpu, pcm_sel[k], pcm_len);
        for (int k = 0; k < npeek; k++)
            printf("  ds:[%04X] = %02X  %04X  %08X\n", peek[k],
                   mem_read8(&cpu, EJAY_AUTO_DATA_SEG, peek[k]),
                   mem_read16(&cpu, EJAY_AUTO_DATA_SEG, peek[k]),
                   mem_read32(&cpu, EJAY_AUTO_DATA_SEG, peek[k]));
    }

    if (wav) ejay_wave_dump_close();
    if (shot)
        printf("%s %s\n", ejay_video_save(shot) ? "wrote" : "could not write", shot);
    if (window) ejay_video_close();
    cpu_free(&cpu);
    return 0;
}
