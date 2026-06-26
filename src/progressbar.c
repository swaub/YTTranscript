/* ------------------------------------------------------------------ *
 *  progressbar.c -- a custom-drawn progress bar: smooth vertical green
 *  gradient fill overlaid with animated diagonal stripes that scroll while
 *  active, plus a sliding gradient block for marquee/indeterminate mode.
 *
 *  It understands the same messages the rest of the app already sends to
 *  the stock control (PBM_SETPOS / PBM_SETMARQUEE / PBM_SETRANGE32), so
 *  it is a drop-in replacement.  Double-buffered, so it never flickers.
 * ------------------------------------------------------------------ */

#include "common.h"
#include <stdlib.h>

typedef struct {
    int  pos;        /* 0..100                          */
    BOOL marquee;    /* indeterminate                   */
    int  phase;      /* animation counter               */
    BOOL anim;       /* timer running                   */
} PBData;

#define PB_TIMER 1

static void pb_anim(HWND h, PBData *d, BOOL on)
{
    if (on && !d->anim)       { SetTimer(h, PB_TIMER, 33, NULL); d->anim = TRUE; }
    else if (!on && d->anim)  { KillTimer(h, PB_TIMER); d->anim = FALSE; }
}

/* The bar should be animating whenever it is doing something: sliding in
 * marquee mode, or partway through a determinate fill.  Deciding this from
 * BOTH state fields (not just the message that arrived) keeps the timer alive
 * across the SETMARQUEE-then-SETPOS pair that ProgressPct() sends on every
 * update, so the highlight never stalls mid-transcribe. */
static BOOL pb_active(const PBData *d)
{
    return d->marquee || (d->pos > 0 && d->pos < 100);
}

/* Vertical top->bottom gradient inside r (one solid row per scanline). */
static void grad_v(HDC dc, const RECT *r, COLORREF top, COLORREF bot)
{
    int h = r->bottom - r->top;
    if (h <= 0 || r->right <= r->left) return;
    for (int y = 0; y < h; y++) {
        int den = (h > 1) ? (h - 1) : 1;
        int rr = GetRValue(top) + (GetRValue(bot) - GetRValue(top)) * y / den;
        int gg = GetGValue(top) + (GetGValue(bot) - GetGValue(top)) * y / den;
        int bb = GetBValue(top) + (GetBValue(bot) - GetBValue(top)) * y / den;
        RECT row = { r->left, r->top + y, r->right, r->top + y + 1 };
        HBRUSH br = CreateSolidBrush(RGB(rr, gg, bb));
        FillRect(dc, &row, br);
        DeleteObject(br);
    }
}

static void pb_paint(HWND h, PBData *d)
{
    RECT rc; GetClientRect(h, &rc);
    int W = rc.right, H = rc.bottom;
    if (W <= 0 || H <= 0) return;

    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, W, H);
    HBITMAP oldbmp = (HBITMAP)SelectObject(mem, bmp);

    /* track */
    HBRUSH trackBr = CreateSolidBrush(RGB(0xE6, 0xE6, 0xE6));
    FillRect(mem, &rc, trackBr);
    DeleteObject(trackBr);

    const COLORREF cTop = RGB(0x6A, 0xD7, 0x6A);   /* light green */
    const COLORREF cBot = RGB(0x2E, 0x7D, 0x32);   /* dark green  */

    if (d->marquee) {
        int blockW = W * 30 / 100; if (blockW < 50) blockW = 50;
        int span = W + blockW;
        int x = (d->phase * 7) % span - blockW;
        RECT fr = { x < 1 ? 1 : x, 1, (x + blockW > W - 1 ? W - 1 : x + blockW), H - 1 };
        grad_v(mem, &fr, cTop, cBot);
    } else {
        int fillW = (W * d->pos) / 100;
        if (fillW > 1) {
            RECT fr = { 1, 1, fillW, H - 1 };
            grad_v(mem, &fr, cTop, cBot);

            /* Animated diagonal stripes scrolling across the fill -- the
             * familiar "working" look.  They tile the whole filled area (so
             * there is no single band to read as a gap) and their offset is
             * taken ONLY from the animation clock (d->phase), so they glide at
             * a constant rate and can never stall or jump when whisper's
             * reported % leaps forward. */
            int saved = SaveDC(mem);
            IntersectClipRect(mem, 1, 1, fillW, H - 1);
            const int stripeW = 11, period = 28, slant = (H > 3) ? H - 2 : 1;
            int off = (d->phase * 2) % period;
            HBRUSH sbr = CreateSolidBrush(RGB(0x9A, 0xE8, 0x9A));   /* lighter green */
            SelectObject(mem, sbr);
            SelectObject(mem, GetStockObject(NULL_PEN));
            for (int base = off - period; base < fillW + slant; base += period) {
                POINT q[4] = {
                    { base,                   1     },
                    { base + stripeW,         1     },
                    { base + stripeW - slant, H - 1 },
                    { base - slant,           H - 1 },
                };
                Polygon(mem, q, 4);
            }
            RestoreDC(mem, saved);
            DeleteObject(sbr);
        }
    }

    /* 1px border */
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(0xB8, 0xB8, 0xB8));
    HPEN oldpen = (HPEN)SelectObject(mem, pen);
    HBRUSH oldbr = (HBRUSH)SelectObject(mem, GetStockObject(NULL_BRUSH));
    Rectangle(mem, 0, 0, W, H);
    SelectObject(mem, oldbr);
    SelectObject(mem, oldpen);
    DeleteObject(pen);

    BitBlt(dc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldbmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(h, &ps);
}

static LRESULT CALLBACK PbProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    PBData *d = (PBData *)GetWindowLongPtrW(h, GWLP_USERDATA);

    switch (msg) {
    case WM_NCCREATE:
        d = (PBData *)calloc(1, sizeof(PBData));
        if (!d) return FALSE;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)d);
        return DefWindowProcW(h, msg, wp, lp);

    case WM_NCDESTROY:
        if (d) { pb_anim(h, d, FALSE); free(d); SetWindowLongPtrW(h, GWLP_USERDATA, 0); }
        return DefWindowProcW(h, msg, wp, lp);

    case PBM_SETPOS:
        if (d) {
            int p = (int)wp; if (p < 0) p = 0; if (p > 100) p = 100;
            d->pos = p; d->marquee = FALSE;
            pb_anim(h, d, pb_active(d));
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;

    case PBM_SETMARQUEE:
        if (d) {
            d->marquee = (wp != 0);
            pb_anim(h, d, pb_active(d));
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;

    case PBM_SETRANGE32:
    case PBM_SETRANGE:
        return 0;                                /* always 0..100 */

    case WM_TIMER:
        if (d && wp == PB_TIMER) { d->phase++; InvalidateRect(h, NULL, FALSE); }
        return 0;

    case WM_ERASEBKGND:
        return 1;                                /* fully painted in WM_PAINT */

    case WM_PAINT:
        if (d) pb_paint(h, d);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void RegisterProgressBarClass(HINSTANCE hInst)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize        = sizeof wc;
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = PbProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = YTT_PROGRESS_CLASS;
    RegisterClassExW(&wc);
}
