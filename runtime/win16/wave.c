/* wave.c - MMSYSTEM waveOut/waveIn/aux/time, Win16 to Win32.
 *
 * This is where the engine actually makes a noise, and almost none of it can
 * be passed straight through, because the structures on both sides of the call
 * changed shape twice: once from Win16 to Win32, and again from Win32 to x64.
 *
 *   WAVEHDR    Win16: 32 bytes, lpData a far pointer (selector:offset).
 *              x64:   48 bytes, lpData and lpNext 8-byte pointers, dwUser
 *                     8 bytes. Nothing lines up past the first field.
 *   WAVEOUTCAPS Win16: 50 bytes. Win32 added wReserved1 after wChannels, so
 *              dwSupport moved and the struct is 52.
 *   WAVEFORMAT the one piece of luck: same field order and sizes both sides,
 *              because it was always packed.
 *
 * So every header the guest hands us gets a host-side twin, and the two are
 * synchronised field by field at the points the API defines. The guest's audio
 * data does NOT get copied: a selector maps to a flat offset in cpu->mem, and
 * a block allocated as one GlobalAlloc is contiguous there, so the host
 * WAVEHDR's lpData points straight into guest memory and the driver reads the
 * engine's own mixed output.
 *
 * The device is always opened with OUR callback, whatever the guest asked for.
 * A Win16 sampler that waits for a buffer typically spins on WHDR_DONE without
 * pumping a message queue, so the flag has to appear in the guest's own header
 * whether or not the guest asked to be told about it.
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <mmsystem.h>

#include "cpu.h"
#include "runtime_api.h"

#ifdef EJAY_TRACE_WAVE
#define WLOG(...) fprintf(stderr, __VA_ARGS__)
#else
#define WLOG(...) ((void)0)
#endif

static inline uint16_t a16(CPU *cpu, int off) {
    return mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 4 + off));
}
static inline uint32_t a32(CPU *cpu, int off) {
    return (uint32_t)a16(cpu, off) | ((uint32_t)a16(cpu, off + 2) << 16);
}
static inline void ret16(CPU *cpu, int purge, uint16_t ax) {
    cpu->ax = ax; cpu->sp = (uint16_t)(cpu->sp + 4 + purge);
}
static inline void ret32(CPU *cpu, int purge, uint32_t v) {
    cpu->ax = (uint16_t)v; cpu->dx = (uint16_t)(v >> 16);
    cpu->sp = (uint16_t)(cpu->sp + 4 + purge);
}

/* A guest far pointer to a host pointer into the flat image. Valid only for
 * memory inside cpu->mem, which is every selector the guest can name. */
static void *guest_ptr(CPU *cpu, uint16_t seg, uint16_t off) {
    return cpu->mem + cpu->sel_base[seg] + off;
}

/* ---- the Win16 WAVEHDR, as the guest lays it out ---------------------- */
#define W16_HDR_SIZE        32
#define W16_HDR_LPDATA       0   /* far pointer: off16, seg16 */
#define W16_HDR_BUFFERLEN    4
#define W16_HDR_BYTESREC     8
#define W16_HDR_USER        12
#define W16_HDR_FLAGS       16
#define W16_HDR_LOOPS       20

#define MAX_HDRS 64
static struct {
    int      used;
    uint16_t g_seg, g_off;      /* the guest's WAVEHDR */
    WAVEHDR  host;              /* our twin, handed to the driver */
    CPU     *cpu;
} g_hdr[MAX_HDRS];

#define MAX_DEV 8
static struct {
    int       used;
    HWAVEOUT  out;
    HWAVEIN   in;
} g_dev[MAX_DEV];

static int find_hdr(uint16_t seg, uint16_t off) {
    for (int i = 0; i < MAX_HDRS; i++)
        if (g_hdr[i].used && g_hdr[i].g_seg == seg && g_hdr[i].g_off == off)
            return i;
    return -1;
}

/* Pull the guest's header into its twin. Called before prepare and before
 * every write, because the engine rewrites dwBufferLength per buffer. */
static void sync_to_host(CPU *cpu, int i) {
    uint16_t seg = g_hdr[i].g_seg, off = g_hdr[i].g_off;
    uint16_t d_off = mem_read16(cpu, seg, (uint16_t)(off + W16_HDR_LPDATA));
    uint16_t d_seg = mem_read16(cpu, seg, (uint16_t)(off + W16_HDR_LPDATA + 2));
    WAVEHDR *h = &g_hdr[i].host;
    h->lpData          = (LPSTR)guest_ptr(cpu, d_seg, d_off);
    h->dwBufferLength  = mem_read32(cpu, seg, (uint16_t)(off + W16_HDR_BUFFERLEN));
    h->dwBytesRecorded = mem_read32(cpu, seg, (uint16_t)(off + W16_HDR_BYTESREC));
    h->dwUser          = mem_read32(cpu, seg, (uint16_t)(off + W16_HDR_USER));
    h->dwLoops         = mem_read32(cpu, seg, (uint16_t)(off + W16_HDR_LOOPS));
    h->lpNext          = NULL;
    h->reserved        = 0;
}

/* Push the driver's answers back. dwFlags is the one that matters: the engine
 * polls WHDR_DONE on it. */
static void sync_to_guest(CPU *cpu, int i) {
    uint16_t seg = g_hdr[i].g_seg, off = g_hdr[i].g_off;
    WAVEHDR *h = &g_hdr[i].host;
    mem_write32(cpu, seg, (uint16_t)(off + W16_HDR_BYTESREC), h->dwBytesRecorded);
    mem_write32(cpu, seg, (uint16_t)(off + W16_HDR_FLAGS), (uint32_t)h->dwFlags);
}

/* The driver's done notification, on the driver's thread. It writes one dword
 * into guest memory - the flag the engine is spinning on. A wider write from
 * here would race the guest; this one is the whole contract.
 * ponytail: single dword, no lock. Needs one if a shim ever writes more. */
static void CALLBACK wave_done(HWAVEOUT hwo, UINT msg, DWORD_PTR inst,
                               DWORD_PTR p1, DWORD_PTR p2)
{
    (void)hwo; (void)inst; (void)p2;
    if (msg != WOM_DONE) return;
    WAVEHDR *h = (WAVEHDR *)p1;
    for (int i = 0; i < MAX_HDRS; i++) {
        if (g_hdr[i].used && &g_hdr[i].host == h && g_hdr[i].cpu) {
            CPU *cpu = g_hdr[i].cpu;
            mem_write32(cpu, g_hdr[i].g_seg,
                        (uint16_t)(g_hdr[i].g_off + W16_HDR_FLAGS),
                        (uint32_t)h->dwFlags);
            return;
        }
    }
}

/* ===== waveOut ========================================================= */

/* waveOutOpen(LPHWAVEOUT, UINT uDeviceID, LPCWAVEFORMAT, DWORD dwCallback,
 *             DWORD dwInstance, DWORD fdwOpen) - purge 22.
 * The guest's callback request is deliberately ignored: we always take
 * CALLBACK_FUNCTION so WHDR_DONE reaches the guest header even when the guest
 * asked for CALLBACK_NULL and intends to poll. */
void MMSYSTEM_WAVEOUTOPEN(CPU *cpu) {
    uint32_t flags   = a32(cpu, 0);
    uint32_t inst    = a32(cpu, 4);
    uint32_t cb      = a32(cpu, 8);
    uint16_t fmt_off = a16(cpu, 12), fmt_seg = a16(cpu, 14);
    uint16_t devid   = a16(cpu, 16);
    uint16_t h_off   = a16(cpu, 18), h_seg   = a16(cpu, 20);
    (void)inst; (void)cb;

    /* WAVEFORMATEX and the Win16 PCMWAVEFORMAT share a layout for PCM, so the
     * 16 bytes can be read straight out of guest memory. cbSize is ours. */
    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag      = mem_read16(cpu, fmt_seg, fmt_off);
    wf.nChannels       = mem_read16(cpu, fmt_seg, (uint16_t)(fmt_off + 2));
    wf.nSamplesPerSec  = mem_read32(cpu, fmt_seg, (uint16_t)(fmt_off + 4));
    wf.nAvgBytesPerSec = mem_read32(cpu, fmt_seg, (uint16_t)(fmt_off + 8));
    wf.nBlockAlign     = mem_read16(cpu, fmt_seg, (uint16_t)(fmt_off + 12));
    wf.wBitsPerSample  = mem_read16(cpu, fmt_seg, (uint16_t)(fmt_off + 14));
    wf.cbSize          = 0;

    WLOG("[wave] waveOutOpen dev=%u %uHz %uch %ubit flags=%08X\n",
         devid, (unsigned)wf.nSamplesPerSec, wf.nChannels, wf.wBitsPerSample,
         (unsigned)flags);

    /* WAVE_FORMAT_QUERY asks only whether the format is supported. */
    if (flags & WAVE_FORMAT_QUERY) {
        MMRESULT r = waveOutOpen(NULL, devid == 0xFFFF ? WAVE_MAPPER : devid,
                                 &wf, 0, 0, WAVE_FORMAT_QUERY);
        ret16(cpu, 22, (uint16_t)r);
        return;
    }

    int slot = -1;
    for (int i = 0; i < MAX_DEV; i++) if (!g_dev[i].used) { slot = i; break; }
    if (slot < 0) { ret16(cpu, 22, MMSYSERR_ALLOCATED); return; }

    HWAVEOUT hwo = NULL;
    MMRESULT r = waveOutOpen(&hwo, devid == 0xFFFF ? WAVE_MAPPER : devid, &wf,
                             (DWORD_PTR)wave_done, 0, CALLBACK_FUNCTION);
    if (r != MMSYSERR_NOERROR) {
        WLOG("[wave] waveOutOpen failed mmr=%u\n", r);
        ret16(cpu, 22, (uint16_t)r);
        return;
    }
    g_dev[slot].used = 1;
    g_dev[slot].out = hwo;
    if (h_seg) mem_write16(cpu, h_seg, h_off, (uint16_t)(slot + 1));
    ret16(cpu, 22, MMSYSERR_NOERROR);
}

static HWAVEOUT dev_out(uint16_t h) {
    return (h && h <= MAX_DEV && g_dev[h - 1].used) ? g_dev[h - 1].out : NULL;
}

void MMSYSTEM_WAVEOUTCLOSE(CPU *cpu) {
    uint16_t h = a16(cpu, 0);
    HWAVEOUT hwo = dev_out(h);
    MMRESULT r = hwo ? waveOutClose(hwo) : MMSYSERR_INVALHANDLE;
    if (hwo && r == MMSYSERR_NOERROR) { g_dev[h - 1].used = 0; g_dev[h - 1].out = NULL; }
    ret16(cpu, 2, (uint16_t)r);
}

/* waveOutPrepareHeader(HWAVEOUT, LPWAVEHDR, UINT cbwh) - purge 8 */
void MMSYSTEM_WAVEOUTPREPAREHEADER(CPU *cpu) {
    uint16_t cbwh = a16(cpu, 0);
    uint16_t off  = a16(cpu, 2), seg = a16(cpu, 4);
    uint16_t h    = a16(cpu, 6);
    HWAVEOUT hwo = dev_out(h);
    if (!hwo) { ret16(cpu, 8, MMSYSERR_INVALHANDLE); return; }

    /* The guest tells us what it thinks a WAVEHDR is. If that is ever not 32,
     * every offset in this file is wrong and it is better to say so. */
    if (cbwh != W16_HDR_SIZE)
        fprintf(stderr, "[wave] guest sizeof(WAVEHDR)=%u, expected %u - "
                        "the header layout assumed here is wrong\n",
                cbwh, W16_HDR_SIZE);

    int i = find_hdr(seg, off);
    if (i < 0) {
        for (i = 0; i < MAX_HDRS; i++) if (!g_hdr[i].used) break;
        if (i == MAX_HDRS) { ret16(cpu, 8, MMSYSERR_NOMEM); return; }
        memset(&g_hdr[i], 0, sizeof(g_hdr[i]));
        g_hdr[i].used = 1;
        g_hdr[i].g_seg = seg; g_hdr[i].g_off = off;
    }
    g_hdr[i].cpu = cpu;
    sync_to_host(cpu, i);
    MMRESULT r = waveOutPrepareHeader(hwo, &g_hdr[i].host, sizeof(WAVEHDR));
    WLOG("[wave] prepare %04X:%04X len=%u -> %u\n", seg, off,
         (unsigned)g_hdr[i].host.dwBufferLength, r);
    sync_to_guest(cpu, i);
    ret16(cpu, 8, (uint16_t)r);
}

void MMSYSTEM_WAVEOUTUNPREPAREHEADER(CPU *cpu) {
    uint16_t off = a16(cpu, 2), seg = a16(cpu, 4);
    uint16_t h   = a16(cpu, 6);
    HWAVEOUT hwo = dev_out(h);
    int i = find_hdr(seg, off);
    if (!hwo || i < 0) { ret16(cpu, 8, MMSYSERR_INVALHANDLE); return; }
    MMRESULT r = waveOutUnprepareHeader(hwo, &g_hdr[i].host, sizeof(WAVEHDR));
    sync_to_guest(cpu, i);
    if (r == MMSYSERR_NOERROR) g_hdr[i].used = 0;
    ret16(cpu, 8, (uint16_t)r);
}

/* waveOutWrite(HWAVEOUT, LPWAVEHDR, UINT) - purge 8. The engine refills the
 * same header and writes it again, so lpData and dwBufferLength are re-read
 * every time rather than trusted from prepare. */
void MMSYSTEM_WAVEOUTWRITE(CPU *cpu) {
    uint16_t off = a16(cpu, 2), seg = a16(cpu, 4);
    uint16_t h   = a16(cpu, 6);
    HWAVEOUT hwo = dev_out(h);
    int i = find_hdr(seg, off);
    if (!hwo || i < 0) { ret16(cpu, 8, MMSYSERR_INVALHANDLE); return; }

    DWORD flags = g_hdr[i].host.dwFlags;
    sync_to_host(cpu, i);
    g_hdr[i].host.dwFlags = flags & ~WHDR_DONE;   /* prepared, not yet done */
    MMRESULT r = waveOutWrite(hwo, &g_hdr[i].host, sizeof(WAVEHDR));
    WLOG("[wave] write %04X:%04X len=%u -> %u\n", seg, off,
         (unsigned)g_hdr[i].host.dwBufferLength, r);
    sync_to_guest(cpu, i);
    ret16(cpu, 8, (uint16_t)r);
}

void MMSYSTEM_WAVEOUTRESET(CPU *cpu) {
    HWAVEOUT hwo = dev_out(a16(cpu, 0));
    ret16(cpu, 2, (uint16_t)(hwo ? waveOutReset(hwo) : MMSYSERR_INVALHANDLE));
}

/* waveOutGetPosition(HWAVEOUT, LPMMTIME, UINT) - purge 8.
 * MMTIME is { UINT wType; union { DWORD ms/sample/cb; ... } u; } - a WORD then
 * a DWORD in Win16 (6 bytes), a DWORD then a DWORD in Win32. Read the type
 * from the guest's two bytes and write the value back into its four. */
void MMSYSTEM_WAVEOUTGETPOSITION(CPU *cpu) {
    uint16_t off = a16(cpu, 2), seg = a16(cpu, 4);
    uint16_t h   = a16(cpu, 6);
    HWAVEOUT hwo = dev_out(h);
    if (!hwo) { ret16(cpu, 8, MMSYSERR_INVALHANDLE); return; }
    MMTIME mmt;
    memset(&mmt, 0, sizeof(mmt));
    mmt.wType = mem_read16(cpu, seg, off);
    MMRESULT r = waveOutGetPosition(hwo, &mmt, sizeof(mmt));
    if (r == MMSYSERR_NOERROR) {
        mem_write16(cpu, seg, off, (uint16_t)mmt.wType);
        mem_write32(cpu, seg, (uint16_t)(off + 2), (uint32_t)mmt.u.ms);
    }
    ret16(cpu, 8, (uint16_t)r);
}

void MMSYSTEM_WAVEOUTGETVOLUME(CPU *cpu) {
    uint16_t off = a16(cpu, 0), seg = a16(cpu, 2);
    HWAVEOUT hwo = dev_out(a16(cpu, 4));
    DWORD vol = 0;
    MMRESULT r = hwo ? waveOutGetVolume(hwo, &vol) : MMSYSERR_INVALHANDLE;
    /* Only on success. The engine keeps its own default in that DWORD and
     * reads it straight back, so a zero written on failure mutes it. */
    if (r == MMSYSERR_NOERROR && seg) mem_write32(cpu, seg, off, (uint32_t)vol);
    ret16(cpu, 6, (uint16_t)r);
}

void MMSYSTEM_WAVEOUTSETVOLUME(CPU *cpu) {
    uint32_t vol = a32(cpu, 0);
    HWAVEOUT hwo = dev_out(a16(cpu, 4));
    ret16(cpu, 6, (uint16_t)(hwo ? waveOutSetVolume(hwo, vol) : MMSYSERR_INVALHANDLE));
}

void MMSYSTEM_WAVEOUTGETID(CPU *cpu) {
    uint16_t off = a16(cpu, 0), seg = a16(cpu, 2);
    HWAVEOUT hwo = dev_out(a16(cpu, 4));
    UINT id = 0;
    MMRESULT r = hwo ? waveOutGetID(hwo, &id) : MMSYSERR_INVALHANDLE;
    if (r == MMSYSERR_NOERROR && seg) mem_write16(cpu, seg, off, (uint16_t)id);
    ret16(cpu, 6, (uint16_t)r);
}

/* waveOutGetDevCaps(UINT, LPWAVEOUTCAPS, UINT) - purge 8.
 * Win16 WAVEOUTCAPS is 50 bytes and has no wReserved1, so dwSupport sits four
 * bytes earlier than in the Win32 struct. Copied field by field. */
void MMSYSTEM_WAVEOUTGETDEVCAPS(CPU *cpu) {
    uint16_t off = a16(cpu, 2), seg = a16(cpu, 4);
    uint16_t id  = a16(cpu, 6);
    WAVEOUTCAPSA caps;
    memset(&caps, 0, sizeof(caps));
    MMRESULT r = waveOutGetDevCapsA(id == 0xFFFF ? WAVE_MAPPER : id,
                                    &caps, sizeof(caps));
    if (r == MMSYSERR_NOERROR && seg) {
        mem_write16(cpu, seg, off, caps.wMid);
        mem_write16(cpu, seg, (uint16_t)(off + 2), caps.wPid);
        mem_write32(cpu, seg, (uint16_t)(off + 4), caps.vDriverVersion);
        for (int i = 0; i < 32; i++)
            mem_write8(cpu, seg, (uint16_t)(off + 8 + i), (uint8_t)caps.szPname[i]);
        mem_write32(cpu, seg, (uint16_t)(off + 40), caps.dwFormats);
        mem_write16(cpu, seg, (uint16_t)(off + 44), caps.wChannels);
        mem_write32(cpu, seg, (uint16_t)(off + 46), caps.dwSupport);
    }
    ret16(cpu, 8, (uint16_t)r);
}

/* ===== aux: the master volume ========================================== */
void MMSYSTEM_AUXGETVOLUME(CPU *cpu) {
    uint16_t off = a16(cpu, 0), seg = a16(cpu, 2);
    uint16_t id  = a16(cpu, 4);
    DWORD vol = 0;
    MMRESULT r = auxGetVolume(id, &vol);
    if (r == MMSYSERR_NOERROR && seg) mem_write32(cpu, seg, off, (uint32_t)vol);
    ret16(cpu, 6, (uint16_t)r);
}

void MMSYSTEM_AUXSETVOLUME(CPU *cpu) {
    uint32_t vol = a32(cpu, 0);
    ret16(cpu, 6, (uint16_t)auxSetVolume(a16(cpu, 4), vol));
}

/* auxGetDevCaps(UINT, LPAUXCAPS, UINT) - purge 8. Win16 AUXCAPS, like
 * WAVEOUTCAPS, has no wReserved1: 2+2+4+32+2+4+4 with wTechnology where
 * dwFormats would be. */
void MMSYSTEM_AUXGETDEVCAPS(CPU *cpu) {
    uint16_t off = a16(cpu, 2), seg = a16(cpu, 4);
    uint16_t id  = a16(cpu, 6);
    AUXCAPSA caps;
    memset(&caps, 0, sizeof(caps));
    MMRESULT r = auxGetDevCapsA(id, &caps, sizeof(caps));
    if (r == MMSYSERR_NOERROR && seg) {
        mem_write16(cpu, seg, off, caps.wMid);
        mem_write16(cpu, seg, (uint16_t)(off + 2), caps.wPid);
        mem_write32(cpu, seg, (uint16_t)(off + 4), caps.vDriverVersion);
        for (int i = 0; i < 32; i++)
            mem_write8(cpu, seg, (uint16_t)(off + 8 + i), (uint8_t)caps.szPname[i]);
        mem_write16(cpu, seg, (uint16_t)(off + 40), caps.wTechnology);
        mem_write32(cpu, seg, (uint16_t)(off + 42), caps.dwSupport);
    }
    ret16(cpu, 8, (uint16_t)r);
}

/* ===== waveIn: recording ==============================================
 * ARecInit/ARecStart/ARecInput are the sampler's record path. Opened the same
 * way as playback, with our callback and host-side twins. */
static HWAVEIN dev_in(uint16_t h) {
    return (h && h <= MAX_DEV && g_dev[h - 1].used) ? g_dev[h - 1].in : NULL;
}

void MMSYSTEM_WAVEINOPEN(CPU *cpu) {
    uint16_t fmt_off = a16(cpu, 12), fmt_seg = a16(cpu, 14);
    uint16_t devid   = a16(cpu, 16);
    uint16_t h_off   = a16(cpu, 18), h_seg   = a16(cpu, 20);

    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag      = mem_read16(cpu, fmt_seg, fmt_off);
    wf.nChannels       = mem_read16(cpu, fmt_seg, (uint16_t)(fmt_off + 2));
    wf.nSamplesPerSec  = mem_read32(cpu, fmt_seg, (uint16_t)(fmt_off + 4));
    wf.nAvgBytesPerSec = mem_read32(cpu, fmt_seg, (uint16_t)(fmt_off + 8));
    wf.nBlockAlign     = mem_read16(cpu, fmt_seg, (uint16_t)(fmt_off + 12));
    wf.wBitsPerSample  = mem_read16(cpu, fmt_seg, (uint16_t)(fmt_off + 14));

    int slot = -1;
    for (int i = 0; i < MAX_DEV; i++) if (!g_dev[i].used) { slot = i; break; }
    if (slot < 0) { ret16(cpu, 22, MMSYSERR_ALLOCATED); return; }

    HWAVEIN hwi = NULL;
    MMRESULT r = waveInOpen(&hwi, devid == 0xFFFF ? WAVE_MAPPER : devid, &wf,
                            0, 0, CALLBACK_NULL);
    if (r != MMSYSERR_NOERROR) { ret16(cpu, 22, (uint16_t)r); return; }
    g_dev[slot].used = 1;
    g_dev[slot].in = hwi;
    if (h_seg) mem_write16(cpu, h_seg, h_off, (uint16_t)(slot + 1));
    ret16(cpu, 22, MMSYSERR_NOERROR);
}

void MMSYSTEM_WAVEINCLOSE(CPU *cpu) {
    uint16_t h = a16(cpu, 0);
    HWAVEIN hwi = dev_in(h);
    MMRESULT r = hwi ? waveInClose(hwi) : MMSYSERR_INVALHANDLE;
    if (hwi && r == MMSYSERR_NOERROR) { g_dev[h - 1].used = 0; g_dev[h - 1].in = NULL; }
    ret16(cpu, 2, (uint16_t)r);
}

void MMSYSTEM_WAVEINPREPAREHEADER(CPU *cpu) {
    uint16_t off = a16(cpu, 2), seg = a16(cpu, 4);
    HWAVEIN hwi = dev_in(a16(cpu, 6));
    if (!hwi) { ret16(cpu, 8, MMSYSERR_INVALHANDLE); return; }
    int i = find_hdr(seg, off);
    if (i < 0) {
        for (i = 0; i < MAX_HDRS; i++) if (!g_hdr[i].used) break;
        if (i == MAX_HDRS) { ret16(cpu, 8, MMSYSERR_NOMEM); return; }
        memset(&g_hdr[i], 0, sizeof(g_hdr[i]));
        g_hdr[i].used = 1;
        g_hdr[i].g_seg = seg; g_hdr[i].g_off = off;
    }
    g_hdr[i].cpu = cpu;
    sync_to_host(cpu, i);
    MMRESULT r = waveInPrepareHeader(hwi, &g_hdr[i].host, sizeof(WAVEHDR));
    sync_to_guest(cpu, i);
    ret16(cpu, 8, (uint16_t)r);
}

void MMSYSTEM_WAVEINUNPREPAREHEADER(CPU *cpu) {
    uint16_t off = a16(cpu, 2), seg = a16(cpu, 4);
    HWAVEIN hwi = dev_in(a16(cpu, 6));
    int i = find_hdr(seg, off);
    if (!hwi || i < 0) { ret16(cpu, 8, MMSYSERR_INVALHANDLE); return; }
    MMRESULT r = waveInUnprepareHeader(hwi, &g_hdr[i].host, sizeof(WAVEHDR));
    sync_to_guest(cpu, i);
    if (r == MMSYSERR_NOERROR) g_hdr[i].used = 0;
    ret16(cpu, 8, (uint16_t)r);
}

void MMSYSTEM_WAVEINADDBUFFER(CPU *cpu) {
    uint16_t off = a16(cpu, 2), seg = a16(cpu, 4);
    HWAVEIN hwi = dev_in(a16(cpu, 6));
    int i = find_hdr(seg, off);
    if (!hwi || i < 0) { ret16(cpu, 8, MMSYSERR_INVALHANDLE); return; }
    sync_to_host(cpu, i);
    MMRESULT r = waveInAddBuffer(hwi, &g_hdr[i].host, sizeof(WAVEHDR));
    sync_to_guest(cpu, i);
    ret16(cpu, 8, (uint16_t)r);
}

void MMSYSTEM_WAVEINSTART(CPU *cpu) {
    HWAVEIN hwi = dev_in(a16(cpu, 0));
    ret16(cpu, 2, (uint16_t)(hwi ? waveInStart(hwi) : MMSYSERR_INVALHANDLE));
}
void MMSYSTEM_WAVEINSTOP(CPU *cpu) {
    HWAVEIN hwi = dev_in(a16(cpu, 0));
    ret16(cpu, 2, (uint16_t)(hwi ? waveInStop(hwi) : MMSYSERR_INVALHANDLE));
}
void MMSYSTEM_WAVEINRESET(CPU *cpu) {
    HWAVEIN hwi = dev_in(a16(cpu, 0));
    ret16(cpu, 2, (uint16_t)(hwi ? waveInReset(hwi) : MMSYSERR_INVALHANDLE));
}

/* ===== the multimedia timer ============================================
 * timeSetEvent's callback is a lifted far proc, and a real multimedia timer
 * would call it on the driver's thread - into a CPU model that is one struct
 * with one stack. That is a data race, not a design.
 *
 * So the timer is registered here and NOT started: ejay_timer_pending() and
 * ejay_timer_fire() let the harness run it on the thread that owns the CPU.
 * ponytail: cooperative, driven by the harness. It needs a real thread only if
 * the engine turns out to depend on tick timing rather than tick count.
 */
static struct {
    int      live;
    uint16_t cb_seg, cb_off;    /* the guest's LPTIMECALLBACK */
    uint32_t user;
    uint16_t delay_ms;
    uint16_t flags;
    DWORD    next_due;
} g_timer;

void MMSYSTEM_TIMESETEVENT(CPU *cpu) {
    uint16_t flags  = a16(cpu, 0);
    uint32_t user   = a32(cpu, 2);
    uint16_t cb_off = a16(cpu, 6), cb_seg = a16(cpu, 8);
    uint16_t res    = a16(cpu, 10);
    uint16_t delay  = a16(cpu, 12);
    (void)res;
    g_timer.live = 1;
    g_timer.cb_seg = cb_seg; g_timer.cb_off = cb_off;
    g_timer.user = user;
    g_timer.delay_ms = delay ? delay : 1;
    g_timer.flags = flags;
    g_timer.next_due = timeGetTime() + g_timer.delay_ms;
    WLOG("[wave] timeSetEvent %ums -> %04X:%04X (deferred to the CPU thread)\n",
         delay, cb_seg, cb_off);
    ret16(cpu, 14, 1);          /* timer id */
}

void MMSYSTEM_TIMEKILLEVENT(CPU *cpu) {
    g_timer.live = 0;
    ret16(cpu, 2, TIMERR_NOERROR);
}

int ejay_timer_pending(void) {
    return g_timer.live && (int32_t)(timeGetTime() - g_timer.next_due) >= 0;
}

/* Call the guest's timer callback the way MMSYSTEM would:
 * void CALLBACK TimeProc(UINT id, UINT msg, DWORD user, DWORD, DWORD) */
void ejay_timer_fire(CPU *cpu, void (*dispatch)(CPU *, uint16_t, uint16_t)) {
    if (!g_timer.live) return;
    g_timer.next_due = timeGetTime() + g_timer.delay_ms;
    push16(cpu, 1);                                 /* uID */
    push16(cpu, 0);                                 /* uMsg */
    push16(cpu, (uint16_t)g_timer.user);            /* dwUser lo */
    push16(cpu, (uint16_t)(g_timer.user >> 16));    /* dwUser hi */
    push16(cpu, 0); push16(cpu, 0);                 /* dw1 */
    push16(cpu, 0); push16(cpu, 0);                 /* dw2 */
    push16(cpu, cpu->cs);
    push16(cpu, 0);
    dispatch(cpu, g_timer.cb_seg, g_timer.cb_off);
}
