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

void ejay_fn_hit(const char *n) { (void)n; }

/* Selector-write watchpoint. g_wsel is 0 unless something sets it, so this
 * never fires in a normal run; it exists because "which lifted function
 * scribbled on that selector" is otherwise unanswerable. */
void ejay_sel_write(uint16_t seg, uint16_t off, uint16_t val)
{
    fprintf(stderr, "[sel] write %04X:%04X = %04X from %s\n", seg, off, val,
            g_fn_ring[(g_fn_ring_pos - 1u) & (EJAY_FN_RING_SIZE - 1)]);
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

/* Set the guest up as if a Win16 host were about to make a far call into the
 * DLL: fresh stack, DGROUP in DS, and a far return address on top so the
 * callee's RETF has somewhere to land. */
static void enter_guest(CPU *cpu) {
    cpu->ds = cpu->es = EJAY_AUTO_DATA_SEG;
    cpu->ss = EJAY_STACK_SEG;
    cpu->sp = EJAY_STACK_SP;
    cpu->cs = EJAY_CODE_SEG;
    push16(cpu, 0);            /* return CS: nothing to return to */
    push16(cpu, 0);            /* return IP */
}

int main(int argc, char **argv) {
    const char *img = EJAY_IMAGE_PATH;
    const char *dir = ".";
    const char *call = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--image") && i + 1 < argc) img = argv[++i];
        else call = argv[i];
    }

    SetUnhandledExceptionFilter(crash_handler);
    ejay_set_data_dir(dir);

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
    cpu.di = EJAY_AUTO_DATA_SEG;
    cpu.cx = EJAY_HEAP_SIZE;
    cpu.si = 0;
    seg001_0054(&cpu);
    printf("\nLibMain returned ax=%04X (%s)\n", cpu.ax,
           cpu.ax ? "success" : "FAILED - the DLL refused to initialise");

    if (call) {
        const EjayExport *e = find_export(call);
        if (!e) {
            fprintf(stderr, "no such export: %s\n", call);
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
        printf("\ncalling %s (@%d) ...\n", e->name, e->ordinal);
        fflush(stdout);
        enter_guest(&cpu);
        e->fn(&cpu);
        printf("%s returned ax=%04X dx=%04X\n", e->name, cpu.ax, cpu.dx);
    } else {
        list_exports();
    }

    cpu_free(&cpu);
    return 0;
}
