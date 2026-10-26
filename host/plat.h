/* plat.h - what host/ejay2.c needs from the platform, on Windows and off it.
 *
 * On Windows the title's DLLs are loaded and run as themselves, and every
 * Win32 call is the real one. Off Windows (Linux, macOS) the DLLs are lifted
 * to C (tools/lift_ejay2.py) and run on pcrecomp's runtime/win32hle, which
 * answers their Win32 calls itself - and this host's too, so the window, the
 * DCs and the bitmaps it hands the DLLs are the same objects theirs are.
 *
 * The one rule that makes both work from one source: anything the DLLs or
 * Win32 are given a pointer to lives in guest memory. On Windows that is any
 * memory; off Windows the guest is a 4 GB block of a 64-bit process, so
 *
 *   gmem(n)       allocates in it (calloc on Windows),
 *   GVA(p)        is a host pointer's guest address (stops the run if p is
 *                 not in guest memory, rather than truncating it),
 *   HOSTP(va)      is a guest address as a host pointer,
 *   GSTR(s)       copies a short-lived string in (a ring, reused),
 *   X(fn, ...)    calls a DLL export at guest address fn, stdcall, with
 *                 32-bit arguments.
 *
 * Win32 calls with only integer and handle arguments are written as usual;
 * off Windows each is a macro onto win32hle's shim of the same name. The few
 * that take pointers go through the helpers at the bottom of this file. See
 * docs/PORTING.md.
 */
#ifndef EJAY_PLAT_H
#define EJAY_PLAT_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t gva;                      /* an address in the guest's 32-bit space */

#define X(fn, ...) xcall((fn), (int)(sizeof((uint32_t[]){ __VA_ARGS__ }) / 4), (uint32_t[]){ __VA_ARGS__ })
#define X0(fn)     xcall((fn), 0, NULL)

#ifdef _WIN32
/* ======================================================================= Windows */
#include <windows.h>
#include <mmsystem.h>

#define GVA(p)   ((gva)(uintptr_t)(p))
#define HOSTP(va) ((void *)(uintptr_t)(va))
#define GSTR(s)  GVA(s)
static void *gmem(size_t n) { return calloc(1, n); }

/* Every export the host calls is __stdcall with 32-bit arguments, so one
 * switch on the count covers them. */
typedef uint32_t u32;
static uint32_t xcall(gva fn, int n, const uint32_t *a)
{
    if (!fn) return 0;
    switch (n) {
    case 0:  return ((u32 (__stdcall *)(void))fn)();
    case 1:  return ((u32 (__stdcall *)(u32))fn)(a[0]);
    case 2:  return ((u32 (__stdcall *)(u32, u32))fn)(a[0], a[1]);
    case 3:  return ((u32 (__stdcall *)(u32, u32, u32))fn)(a[0], a[1], a[2]);
    case 4:  return ((u32 (__stdcall *)(u32, u32, u32, u32))fn)(a[0], a[1], a[2], a[3]);
    case 5:  return ((u32 (__stdcall *)(u32, u32, u32, u32, u32))fn)(a[0], a[1], a[2], a[3], a[4]);
    case 6:  return ((u32 (__stdcall *)(u32, u32, u32, u32, u32, u32))fn)(a[0], a[1], a[2], a[3], a[4], a[5]);
    case 7:  return ((u32 (__stdcall *)(u32, u32, u32, u32, u32, u32, u32))fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6]);
    case 11: return ((u32 (__stdcall *)(u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32))fn)
                 (a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10]);
    case 12: return ((u32 (__stdcall *)(u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32))fn)
                 (a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11]);
    }
    abort();                               /* a count this table lacks: add it */
}

static gva load_dll(const char *name) { return GVA(LoadLibraryA(name)); }
static gva dll_proc(gva mod, const char *name) { return GVA(GetProcAddress((HMODULE)HOSTP(mod), name)); }
static FILE *xfopen(const char *path, const char *mode) { return fopen(path, mode); }
static void plat_sleep(int ms) { Sleep((DWORD)ms); }
static uint32_t plat_ticks(void) { return GetTickCount(); }

/* A DIB section with the given BITMAPINFO; its pixels' address in *bits. */
static HBITMAP dib_section(HDC dc, const void *bmi, gva *bits)
{
    void *p = NULL;
    HBITMAP b = CreateDIBSection(dc, (const BITMAPINFO *)bmi, DIB_RGB_COLORS, &p, NULL, 0);
    *bits = GVA(p);
    return b;
}
#define TEXTOUT(dc, x, y, s)   TextOutA((dc), (x), (y), (s), (int)strlen(s))
#define FILLRECT(dc, r, br)    FillRect((dc), (r), (br))
#define FONT(h, face)          CreateFontA((h), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, DEFAULT_QUALITY, 0, (face))

#else
/* ================================================================= Linux, macOS */
#include <stdio.h>
#include "win32hle.h"

#define GVA(p)   hle_va(p)
#define HOSTP(va) hle_ptr(va)
static void *gmem(size_t n) { return hle_alloc((uint32_t)n); }
gva gcopy(const void *p, size_t n);        /* posix.c: into a ring, reused */
#define GSTR(s) gcopy((s), strlen(s) + 1)
uint32_t xcall(gva fn, int n, const uint32_t *a);
gva api(const char *name);                 /* a win32hle shim's address, by name */
gva load_dll(const char *name);
gva dll_proc(gva mod, const char *name);
FILE *xfopen(const char *path, const char *mode);   /* a Windows path, any case */
void plat_sleep(int ms);
uint32_t plat_ticks(void);

typedef uint32_t HWND, HDC, HBITMAP, HFONT, HBRUSH, HRGN, HGDIOBJ, HMODULE, HCURSOR;
typedef uint32_t UINT, DWORD, WPARAM, LPARAM, LRESULT, COLORREF;
typedef int32_t  LONG;
typedef intptr_t INT_PTR;
typedef struct { LONG left, top, right, bottom; } RECT;
#define CALLBACK
#define MAX_PATH 260

/* A Win32 call onto win32hle's shim, its address looked up once per call site. */
#define W(name, ...) ({ static gva va_; if (!va_) va_ = api(#name); X(va_, __VA_ARGS__); })
#define W0(name)     ({ static gva va_; if (!va_) va_ = api(#name); X0(va_); })

#define BitBlt(d, x, y, w, h, s, sx, sy, rop) W(BitBlt, d, x, y, w, h, s, sx, sy, rop)
#define PatBlt(d, x, y, w, h, rop)            W(PatBlt, d, x, y, w, h, rop)
#define GetDC(h)                              W(GetDC, h)
#define ReleaseDC(h, dc)                      W(ReleaseDC, h, dc)
#define SelectObject(dc, o)                   W(SelectObject, dc, o)
#define DeleteObject(o)                       W(DeleteObject, o)
#define DeleteDC(dc)                          W(DeleteDC, dc)
#define SetTextColor(dc, c)                   W(SetTextColor, dc, c)
#define SetBkMode(dc, m)                      W(SetBkMode, dc, m)
#define CreateRectRgn(l, t, r, b)             W(CreateRectRgn, l, t, r, b)
#define SelectClipRgn(dc, rgn)                W(SelectClipRgn, dc, rgn)
#define CreateSolidBrush(c)                   W(CreateSolidBrush, c)
#define CreateCompatibleDC(dc)                W(CreateCompatibleDC, dc)
#define CreateCompatibleBitmap(dc, w, h)      W(CreateCompatibleBitmap, dc, w, h)
#define SetCapture(h)                         W(SetCapture, h)
#define ReleaseCapture()                      W0(ReleaseCapture)
#define IsWindow(h)                           W(IsWindow, h)
#define DefWindowProcA(h, m, w, l)            W(DefWindowProcA, h, m, w, l)
#define PostQuitMessage(c)                    W(PostQuitMessage, c)
#define EndPaint(h, ps)                       W(EndPaint, h, GVA(ps))
#define BeginPaint(h, ps)                     W(BeginPaint, h, GVA(ps))
#define TEXTOUT(dc, x, y, s)   W(TextOutA, dc, x, y, GSTR(s), (uint32_t)strlen(s))
#define FILLRECT(dc, r, br)    W(FillRect, dc, gcopy((r), sizeof(RECT)), br)
#define FONT(h, face)          W(CreateFontA, (uint32_t)(h), 0, 0, 0, 400, 0, 0, 0, 1, 0, 0, 0, 0, GSTR(face))
HBITMAP dib_section(HDC dc, const void *bmi, gva *bits);

typedef struct { uint8_t b[64]; } PAINTSTRUCT;
#define SRCCOPY     0x00CC0020u
#define BLACKNESS   0x00000042u
#define WHITENESS   0x00FF0062u
#define TRANSPARENT 1
#define WM_DESTROY       0x0002
#define WM_PAINT         0x000F
#define WM_ERASEBKGND    0x0014
#define WM_MOUSEMOVE     0x0200
#define WM_LBUTTONDOWN   0x0201
#define WM_LBUTTONUP     0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_MOUSEWHEEL    0x020A
#define WHEEL_DELTA      120
#define RGB(r, g, b)     ((COLORREF)((r) | ((g) << 8) | ((b) << 16)))
#define LOWORD(l)        ((uint16_t)((l) & 0xFFFF))
#define HIWORD(l)        ((uint16_t)(((l) >> 16) & 0xFFFF))
#define MAKELPARAM(a, b) ((LPARAM)(((uint32_t)(uint16_t)(a)) | ((uint32_t)(uint16_t)(b) << 16)))
#define GET_WHEEL_DELTA_WPARAM(w) ((short)HIWORD(w))
#endif

/* --------------------------------------------------------------- both */
/* The window, the message pump and the audio-session guard differ more than
 * a macro can cover: each platform's file has its own (host/win32.c,
 * host/posix.c), and ejay2.c calls these. */
typedef LRESULT (CALLBACK *wndproc_t)(HWND, UINT, WPARAM, LPARAM);
HWND  plat_window(int w, int h, const char *title, wndproc_t proc);
void  plat_pump(void);
int   plat_open(int argc, char **argv);   /* before anything else; nonzero to stop */
void  plat_audio_guard(int percent);      /* hold output muted until plat_audio_armed(1) */
void  plat_audio_armed(int armed);
float plat_peak(void);                    /* the level leaving the endpoint, or 0 if unknown */
int   plat_headless(void);
const char *plat_lib_root(const char *libdir);   /* the library folder, as the engine will name it */
int   plat_list(const char *dir, const char *ext, char names[][64], int max);   /* files ending ext, any case */
const char *plat_cache(void);           /* a folder to write into, as the engine names it */

#endif
