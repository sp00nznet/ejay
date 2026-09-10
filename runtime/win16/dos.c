/* dos.c - the INT 21h calls Borland's runtime makes on the engine's behalf.
 *
 * KERNEL.DOS3Call is a raw INT 21h: no arguments are pushed, everything is in
 * registers, and the caller reads CF for success. Borland's C runtime routes
 * findfirst/findnext through it, and that is how Dance eJay discovers its
 * sample library - it does not read an index file, it walks the disc.
 *
 * The engine builds two-letter directory names (`ba` through `bw`, then the
 * `a` range) and asks DOS what is inside each. With every call failing, the
 * 1600-slot table at DGROUP 0x121E stays empty and every sample name comes out
 * as `aaaa.pxd`. So these five functions are the whole difference between an
 * engine that knows its library and one that does not.
 *
 * The search state lives in the DOS Disk Transfer Address, a 43-byte structure
 * the caller relocates with AH=1A and reads back with AH=2F:
 *
 *     0x00  21 bytes  reserved - DOS keeps its search state here
 *     0x15  byte      attributes of the file found
 *     0x16  word      time
 *     0x18  word      date
 *     0x1A  dword     size
 *     0x1E  13 bytes  ASCIIZ name, 8.3
 *
 * We keep the real Win32 search handle in a side table keyed by the DTA's
 * address rather than in those reserved bytes, because a HANDLE does not fit
 * in the space DOS used and the guest never looks inside them anyway.
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "cpu.h"
#include "runtime_api.h"

#ifdef EJAY_TRACE_DOS
#define DLOG(...) fprintf(stderr, __VA_ARGS__)
#else
#define DLOG(...) ((void)0)
#endif

void ejay_data_dir(char *out, int max);      /* from win16_impl.c */
FILE *ejay_hfile(uint16_t h);                /* from win16_impl.c */

/* ---- the DTA ---------------------------------------------------------- */
#define DTA_ATTR   0x15
#define DTA_TIME   0x16
#define DTA_DATE   0x18
#define DTA_SIZE   0x1A
#define DTA_NAME   0x1E

static uint16_t g_dta_seg, g_dta_off;

#define MAX_SEARCH 16
static struct {
    int      used;
    uint32_t key;              /* the DTA address the search belongs to */
    HANDLE   h;
} g_search[MAX_SEARCH];

static int find_search(uint32_t key) {
    for (int i = 0; i < MAX_SEARCH; i++)
        if (g_search[i].used && g_search[i].key == key) return i;
    return -1;
}

static void close_search(int i) {
    if (i >= 0 && g_search[i].used) {
        if (g_search[i].h != INVALID_HANDLE_VALUE) FindClose(g_search[i].h);
        g_search[i].used = 0;
    }
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

/* DOS reports names in 8.3, upper case, and the engine parses the name it gets
 * back - so hand it the short name rather than whatever case the host uses. */
static void write_found(CPU *cpu, const WIN32_FIND_DATAA *fd) {
    uint16_t seg = g_dta_seg, off = g_dta_off;
    const char *name = fd->cAlternateFileName[0] ? fd->cAlternateFileName
                                                 : fd->cFileName;
    uint8_t attr = 0;
    if (fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) attr |= 0x10;
    if (fd->dwFileAttributes & FILE_ATTRIBUTE_READONLY)  attr |= 0x01;
    if (fd->dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)    attr |= 0x02;
    if (fd->dwFileAttributes & FILE_ATTRIBUTE_SYSTEM)    attr |= 0x04;

    mem_write8(cpu, seg, (uint16_t)(off + DTA_ATTR), attr);

    WORD fdate = 0, ftime = 0;
    FILETIME lft;
    if (FileTimeToLocalFileTime(&fd->ftLastWriteTime, &lft))
        FileTimeToDosDateTime(&lft, &fdate, &ftime);
    mem_write16(cpu, seg, (uint16_t)(off + DTA_TIME), ftime);
    mem_write16(cpu, seg, (uint16_t)(off + DTA_DATE), fdate);
    mem_write32(cpu, seg, (uint16_t)(off + DTA_SIZE), fd->nFileSizeLow);

    int i = 0;
    for (; i < 12 && name[i]; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        mem_write8(cpu, seg, (uint16_t)(off + DTA_NAME + i), (uint8_t)c);
    }
    mem_write8(cpu, seg, (uint16_t)(off + DTA_NAME + i), 0);
    DLOG("[dos]   -> %s (%lu bytes)\n", name, (unsigned long)fd->nFileSizeLow);
}

/* DOS3Call takes no arguments, but it is still reached by a FAR CALL, so
 * the 4-byte return address the lifter pushed has to come off here - the
 * C return is the RETF. Leaking it costs 4 bytes of guest stack per INT
 * 21h, which is invisible for a few calls and then shifts every local the
 * caller owns: Borland's findnext wrapper started reading its own result
 * from the wrong slot, returned "success" forever, and the directory scan
 * never terminated. Twenty million calls before anyone noticed. */
static void ok(CPU *cpu) {
    cpu->flags &= ~FLAG_CF;
    cpu->sp = (uint16_t)(cpu->sp + 4);
}
static void err(CPU *cpu, uint16_t code) {
    cpu->flags |= FLAG_CF;
    cpu->ax = code;
    cpu->sp = (uint16_t)(cpu->sp + 4);
}

void KERNEL_DOS3CALL(CPU *cpu)
{
    uint8_t ah = (uint8_t)(cpu->ax >> 8);

    switch (ah) {
    case 0x30:              /* get DOS version - AL major, AH minor */
        cpu->ax = 0x1606;   /* 6.22, the last real one */
        cpu->bx = 0; cpu->cx = 0;
        ok(cpu);
        return;

    case 0x2F:              /* get DTA -> ES:BX */
        cpu->es = g_dta_seg;
        cpu->bx = g_dta_off;
        ok(cpu);
        return;

    case 0x1A:              /* set DTA = DS:DX */
        g_dta_seg = cpu->ds;
        g_dta_off = cpu->dx;
        ok(cpu);
        return;

    case 0x4E: {            /* find first: DS:DX = pattern, CX = attributes */
        char pat[MAX_PATH], path[MAX_PATH * 2], dir[MAX_PATH];
        read_asciiz(cpu, cpu->ds, cpu->dx, pat, sizeof(pat));

        /* The engine searches with relative patterns like `ba\*.pxd`, so they
         * resolve against the data directory, exactly as OpenFile does. */
        if (pat[0] && (pat[1] == ':' || pat[0] == '\\' || pat[0] == '/')) {
            snprintf(path, sizeof(path), "%s", pat);
        } else {
            ejay_data_dir(dir, sizeof(dir));
            snprintf(path, sizeof(path), "%s\\%s", dir[0] ? dir : ".", pat);
        }

        uint32_t key = ((uint32_t)g_dta_seg << 16) | g_dta_off;
        int i = find_search(key);
        if (i >= 0) close_search(i);
        for (i = 0; i < MAX_SEARCH; i++) if (!g_search[i].used) break;
        if (i == MAX_SEARCH) { err(cpu, 4); return; }   /* too many open files */

        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(path, &fd);
        DLOG("[dos] findfirst %s -> %s (DTA %04X:%04X)\n", path,
             h == INVALID_HANDLE_VALUE ? "none" : "ok", g_dta_seg, g_dta_off);
        if (h == INVALID_HANDLE_VALUE) { err(cpu, 18); return; }  /* no more files */

        g_search[i].used = 1;
        g_search[i].key = key;
        g_search[i].h = h;
        write_found(cpu, &fd);
        ok(cpu);
        return;
    }

    case 0x4F: {            /* find next, using the DTA set by find first */
        uint32_t key = ((uint32_t)g_dta_seg << 16) | g_dta_off;
        int i = find_search(key);
        if (i < 0) {
            static int moaned;
            if (moaned++ < 5)
                fprintf(stderr, "[dos] findnext, no live search for DTA %04X:%04X\n",
                        g_dta_seg, g_dta_off);
            err(cpu, 18); return;
        }

        WIN32_FIND_DATAA fd;
        if (!FindNextFileA(g_search[i].h, &fd)) {
            DLOG("[dos] findnext -> no more files\n");
            close_search(i);
            err(cpu, 18);
            return;
        }
        write_found(cpu, &fd);
        ok(cpu);
        return;
    }

    case 0x42: {            /* lseek: BX handle, CX:DX offset, AL origin */
        FILE *f = ejay_hfile(cpu->bx);
        if (!f) { err(cpu, 6); return; }    /* invalid handle */
        long off = (long)(((uint32_t)cpu->cx << 16) | cpu->dx);
        int whence = (cpu->ax & 0xFF) == 1 ? SEEK_CUR
                   : (cpu->ax & 0xFF) == 2 ? SEEK_END : SEEK_SET;
        if (fseek(f, off, whence) != 0) { err(cpu, 25); return; }
        long pos = ftell(f);
        DLOG("[dos] lseek h=%u %ld whence=%d -> %ld\n", cpu->bx, off, whence, pos);
        cpu->ax = (uint16_t)pos;
        cpu->dx = (uint16_t)((uint32_t)pos >> 16);
        ok(cpu);
        return;
    }

    case 0x3E:              /* close - OpenFile/_lclose own the table, so this
                             * only has to not fail */
        ok(cpu);
        return;

    case 0x3F: {            /* read: BX handle, CX bytes, DS:DX buffer */
        FILE *f = ejay_hfile(cpu->bx);
        if (!f) { err(cpu, 6); return; }
        uint16_t want = cpu->cx, got = 0;
        uint8_t chunk[4096];
        while (got < want) {
            size_t n = want - got < sizeof(chunk) ? (size_t)(want - got) : sizeof(chunk);
            size_t r = fread(chunk, 1, n, f);
            for (size_t i = 0; i < r; i++)
                mem_write8(cpu, cpu->ds, (uint16_t)(cpu->dx + got + i), chunk[i]);
            got += (uint16_t)r;
            if (r < n) break;
        }
        cpu->ax = got;
        ok(cpu);
        return;
    }

    default:
        /* Everything else still fails, but says so once so an unimplemented
         * function cannot hide the way find-first did. */
        {
            static uint8_t seen[256];
            if (!seen[ah]) {
                seen[ah] = 1;
                fprintf(stderr, "[dos] INT 21h AH=%02X unimplemented\n", ah);
            }
        }
        err(cpu, 1);
        return;
    }
}
