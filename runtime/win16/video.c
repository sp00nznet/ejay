/* video.c - somewhere for the engine to draw.
 *
 * DANCE02 imports exactly one GDI function, BitBlt, and calls it to paint a
 * one-pixel-wide bar:
 *
 *     x = <computed>   y = ds:[0x3146]   w = 1   h = ds:[0x1864] - 2
 *     rop = 0x00FF0062 (WHITENESS)
 *
 * That is the playback position line sweeping across the sample strip - the
 * whole of this engine's user interface. Everything else Dance eJay draws
 * lives in DANCE.EXE, which is VB4 p-code and cannot be lifted.
 *
 * The destination DC is whatever the host handed the engine through ABilder,
 * so it is a number we chose and can map back. Rather than maintain a handle
 * table for a single window, the blit always targets ours:
 * ponytail: one window, guest DC ignored. Give it a real table when a second
 * surface exists.
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "cpu.h"
#include "runtime_api.h"

static HWND  g_wnd;
static HDC   g_memdc;          /* what the engine paints into */
static HBITMAP g_bmp, g_oldbmp;
static int   g_w = 640, g_h = 200;
static int   g_blits;

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (g_memdc) BitBlt(dc, 0, 0, g_w, g_h, g_memdc, 0, 0, SRCCOPY);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, wp, lp);
}

/* Returns the DC value to hand the engine, or 0 if no window was made. */
int ejay_video_open(int width, int height)
{
    g_w = width; g_h = height;
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "ejay_recomp";
    if (!RegisterClassA(&wc)) return 0;

    RECT r = { 0, 0, g_w, g_h };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_wnd = CreateWindowExA(0, "ejay_recomp", "Dance eJay 1997 engine - recompiled",
                            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            r.right - r.left, r.bottom - r.top,
                            NULL, NULL, wc.hInstance, NULL);
    if (!g_wnd) return 0;

    /* The engine paints into a memory DC; WM_PAINT copies it to the window.
     * A 1-pixel bar drawn straight onto the window would vanish on the next
     * repaint, which is exactly the sort of thing that reads as "nothing is
     * happening". */
    HDC dc = GetDC(g_wnd);
    g_memdc = CreateCompatibleDC(dc);
    g_bmp = CreateCompatibleBitmap(dc, g_w, g_h);
    g_oldbmp = (HBITMAP)SelectObject(g_memdc, g_bmp);
    PatBlt(g_memdc, 0, 0, g_w, g_h, BLACKNESS);
    ReleaseDC(g_wnd, dc);

    ShowWindow(g_wnd, SW_SHOW);
    UpdateWindow(g_wnd);
    return 1;
}

void ejay_video_pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    if (g_wnd) InvalidateRect(g_wnd, NULL, FALSE);
}

int ejay_video_blits(void) { return g_blits; }

void ejay_video_close(void)
{
    if (g_memdc) {
        SelectObject(g_memdc, g_oldbmp);
        DeleteObject(g_bmp);
        DeleteDC(g_memdc);
        g_memdc = NULL;
    }
    if (g_wnd) { DestroyWindow(g_wnd); g_wnd = NULL; }
}

/* ---- the one GDI call the engine makes ---------------------------------
 * BitBlt(hdcDest, x, y, w, h, hdcSrc, xSrc, ySrc, rop) - PASCAL, purge 20,
 * arguments pushed left to right so hdcDest is deepest. */
static inline uint16_t a16(CPU *cpu, int off) {
    return mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 4 + off));
}

void GDI_BITBLT(CPU *cpu)
{
    uint32_t rop  = (uint32_t)a16(cpu, 0) | ((uint32_t)a16(cpu, 2) << 16);
    int16_t  ysrc = (int16_t)a16(cpu, 4);
    int16_t  xsrc = (int16_t)a16(cpu, 6);
    uint16_t hsrc = a16(cpu, 8);
    int16_t  h    = (int16_t)a16(cpu, 10);
    int16_t  w    = (int16_t)a16(cpu, 12);
    int16_t  y    = (int16_t)a16(cpu, 14);
    int16_t  x    = (int16_t)a16(cpu, 16);
    uint16_t hdst = a16(cpu, 18);
    (void)hsrc; (void)hdst;

    g_blits++;
    if (g_memdc && w > 0 && h > 0) {
        /* WHITENESS and BLACKNESS need no source, which is the whole of what
         * this engine asks for. Anything else is reported rather than guessed
         * at, so a second drawing mode cannot arrive unnoticed. */
        if (rop == WHITENESS || rop == BLACKNESS) {
            PatBlt(g_memdc, x, y, w, h, rop);
        } else if (rop == SRCCOPY) {
            /* The engine copies from a DC the host gave it, and the only
             * surface here is ours - so a SRCCOPY is the display scrolling or
             * restoring part of itself. Self-copy is the honest reading of
             * that; a real second surface would need a handle table.
             * ponytail: self-copy. Revisit when the host owns two DCs. */
            BitBlt(g_memdc, x, y, w, h, g_memdc, xsrc, ysrc, SRCCOPY);
        } else {
            static int moaned;
            if (!moaned++)
                fprintf(stderr, "[gdi] BitBlt rop %08lX not handled\n",
                        (unsigned long)rop);
        }
    }
    cpu->ax = 1;
    cpu->sp = (uint16_t)(cpu->sp + 4 + 20);
}
