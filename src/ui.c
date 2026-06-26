/* ------------------------------------------------------------------ *
 *  ui.c  --  WndProc and all UI-thread logic for YTTranscript.exe.
 *
 *  Owns every HWND.  Builds the controls in WM_CREATE, lays them out
 *  DPI-scaled in LayoutChildren, dispatches the Transcribe/Summarize/
 *  Copy/Cancel buttons, and consumes the WM_APP_* messages the worker
 *  threads post.  Every heap WCHAR* payload that arrives via lParam is
 *  freed here (the UI thread is its sole owner).  SendMessage is never
 *  used to talk to workers; workers only PostMessageW back to us.
 * ------------------------------------------------------------------ */

#include "common.h"
#include <process.h>
#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>

/* Cosmetic label control ids (kept clear of the 1001..1009 control ids
 * in common.h and of the 101/102 resource ids). */
#define IDC_URL_LABEL        2001
#define IDC_TRANSCRIPT_LABEL 2002
#define IDC_SUMMARY_LABEL    2003

/* Single top-level window, so per-window latch state lives module-wide. */
static BOOL g_bootDone       = FALSE;
static BOOL g_haveTranscript = FALSE;
static BOOL g_haveSummary    = FALSE;

/* Running-operation timer: shows elapsed + ETA so long jobs never look stuck. */
#define OP_TIMER_ID 1
static UINT_PTR  g_opTimer     = 0;
static ULONGLONG g_opStartTick = 0;
static int       g_opKind      = 0;     /* 0 none, 1 transcribe, 2 summarize  */
static int       g_opPct       = 0;     /* real % (transcribe, from whisper)   */
static ULONGLONG g_opEstMs     = 0;     /* estimated total ms (summarize)      */
static WCHAR     g_opLabel[64]  = L"";  /* current phase verb                  */

static const WCHAR *const kAssetNames[ASSET_COUNT] = {
    L"yt-dlp",
    L"ffmpeg",
    L"whisper.cpp",
    L"whisper model",
    L"llama.cpp",
    L"summarizer model"
};

/* ----------------------------- helpers ---------------------------- */

static HFONT CreateUiFont(HWND hwnd)
{
    UINT dpi = GetDpiForWindow(hwnd);
    if (dpi == 0)
        dpi = 96;

    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));
    if (!SystemParametersInfoForDpi(SPI_GETICONTITLELOGFONT,
                                    sizeof(lf), &lf, 0, dpi)) {
        lf.lfHeight  = -MulDiv(9, (int)dpi, 72);
        lf.lfWeight  = FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        lstrcpynW(lf.lfFaceName, L"Segoe UI", LF_FACESIZE);
    }

    HFONT f = CreateFontIndirectW(&lf);
    if (!f)
        f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    return f;
}

static BOOL CALLBACK SetFontEnumProc(HWND child, LPARAM lp)
{
    SendMessageW(child, WM_SETFONT, (WPARAM)lp, MAKELPARAM(TRUE, 0));
    return TRUE;
}

static void ApplyFontToAll(AppState *app)
{
    if (app->hUiFont)
        EnumChildWindows(app->hMain, SetFontEnumProc, (LPARAM)app->hUiFont);
}

static void ProgressMarquee(AppState *app, BOOL on)
{
    if (app->hProgress)
        SendMessageW(app->hProgress, PBM_SETMARQUEE, (WPARAM)on, 0);
}

static void ProgressPct(AppState *app, int pct)
{
    if (!app->hProgress)
        return;
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    ProgressMarquee(app, FALSE);
    SendMessageW(app->hProgress, PBM_SETPOS, (WPARAM)pct, 0);
}

/* ----------------------- running-operation timer ------------------ */

static void FmtDur(ULONGLONG ms, WCHAR *out, int cap)
{
    unsigned secs = (unsigned)((ms + 999) / 1000);   /* round up */
    unsigned m = secs / 60, s = secs % 60;
    if (m > 0) _snwprintf(out, cap, L"%u:%02u", m, s);
    else       _snwprintf(out, cap, L"%us", s);
    out[cap - 1] = L'\0';
}

static void StripEllipsis(WCHAR *s)
{
    int n = (int)wcslen(s);
    while (n > 0 && (s[n-1] == L'.' || s[n-1] == 0x2026 /* … */ ||
                     s[n-1] == L' ' || s[n-1] == L'\t'))
        s[--n] = L'\0';
}

static int CountWords(const WCHAR *s)
{
    int n = 0; BOOL in = FALSE;
    for (; s && *s; ++s) {
        BOOL sp = (*s == L' ' || *s == L'\t' || *s == L'\r' || *s == L'\n');
        if (!sp && !in) { n++; in = TRUE; }
        else if (sp)    { in = FALSE; }
    }
    return n;
}

/* Words in the transcript box (output of transcribe / input to summarize). */
static int TranscriptWordCount(AppState *app)
{
    int len = GetWindowTextLengthW(app->hTranscript);
    if (len <= 0) return 0;
    WCHAR *t = (WCHAR *)malloc((size_t)(len + 1) * sizeof(WCHAR));
    if (!t) return 0;
    GetWindowTextW(app->hTranscript, t, len + 1);
    int n = CountWords(t);
    free(t);
    return n;
}

static void OpTick(AppState *app)
{
    if (!app || g_opKind == 0)
        return;

    ULONGLONG elapsed = GetTickCount64() - g_opStartTick;
    WCHAR el[16], eta[16], buf[200];
    FmtDur(elapsed, el, ARRAYSIZE(el));

    /* Summarize drives the bar off the time estimate; transcribe's bar is set
     * from whisper's real % by the WM_APP_PROGRESS handler. */
    if (g_opKind == 2) {
        int pct = (g_opEstMs > 0) ? (int)(elapsed * 100 / g_opEstMs) : 0;
        if (pct > 97) pct = 97;                /* hold until the result lands */
        ProgressPct(app, pct);
    }

    /* ETA is ALWAYS (smoothed total estimate − elapsed): because elapsed only
     * grows, the shown time counts DOWN between progress samples instead of
     * sawtoothing.  g_opEstMs is fixed for summarize and EMA-refined for
     * transcribe (updated in the WM_APP_PROGRESS handler). */
    BOOL done = (g_opKind == 1 && g_opPct >= 100);
    if (g_opEstMs > 0 && g_opEstMs > elapsed + 1000 && !done) {
        FmtDur(g_opEstMs - elapsed, eta, ARRAYSIZE(eta));
        if (g_opKind == 1)
            _snwprintf(buf, ARRAYSIZE(buf), L"%s…  %d%%   ~%s left   (%s elapsed)",
                       g_opLabel, g_opPct, eta, el);
        else
            _snwprintf(buf, ARRAYSIZE(buf), L"%s…   ~%s left   (%s elapsed)",
                       g_opLabel, eta, el);
    } else if (g_opKind == 1 && g_opPct > 0 && !done) {
        _snwprintf(buf, ARRAYSIZE(buf), L"%s…  %d%%   (%s elapsed)",
                   g_opLabel, g_opPct, el);
    } else if (g_opEstMs > 0 && !done) {
        _snwprintf(buf, ARRAYSIZE(buf), L"%s…   finishing up   (%s elapsed)",
                   g_opLabel, el);
    } else {
        _snwprintf(buf, ARRAYSIZE(buf), L"%s…   (%s elapsed)", g_opLabel, el);
    }
    buf[ARRAYSIZE(buf) - 1] = L'\0';
    SetWindowTextW(app->hStatus, buf);
}

static void OpBegin(AppState *app, int kind, ULONGLONG estMs)
{
    g_opKind      = kind;
    g_opStartTick = GetTickCount64();
    g_opPct       = 0;
    g_opEstMs     = estMs;
    lstrcpynW(g_opLabel, (kind == 1) ? L"Transcribing" : L"Summarizing",
              ARRAYSIZE(g_opLabel));
    ProgressPct(app, 0);
    if (!g_opTimer)
        g_opTimer = SetTimer(app->hMain, OP_TIMER_ID, 500, NULL);
    OpTick(app);
}

static void OpEnd(AppState *app)
{
    g_opKind = 0;
    if (g_opTimer) {
        KillTimer(app->hMain, OP_TIMER_ID);
        g_opTimer = 0;
    }
}

static void UpdateButtons(AppState *app)
{
    BOOL busy = (app->busy != 0);
    EnableWindow(app->hUrl,        !busy);
    EnableWindow(app->hTranscribe, !busy && g_bootDone);
    EnableWindow(app->hSummarize,  !busy && g_bootDone && g_haveTranscript);
    EnableWindow(app->hCopy,       !busy && g_haveSummary);
    EnableWindow(app->hCancel,      busy);
}

static void WorkerFinished(AppState *app)
{
    OpEnd(app);
    InterlockedExchange(&app->busy, 0);
    if (app->hWorker) {
        CloseHandle(app->hWorker);
        app->hWorker = NULL;
    }
    UpdateButtons(app);
    if (IsWindowEnabled(app->hUrl))
        SetFocus(app->hUrl);
}

static void StartWorker(AppState *app, int stage,
                        const WCHAR *url, WCHAR *transcript)
{
    if (app->hWorker) {
        CloseHandle(app->hWorker);
        app->hWorker = NULL;
    }

    WorkerArgs *wa = (WorkerArgs *)calloc(1, sizeof(WorkerArgs));
    if (!wa) {
        free(transcript);
        SetWindowTextW(app->hStatus, L"Out of memory starting worker.");
        return;
    }
    wa->hwnd  = app->hMain;
    wa->app   = app;
    wa->stage = stage;
    if (url)
        lstrcpynW(wa->url, url, 2048);
    wa->transcript = transcript;

    InterlockedExchange(&app->cancelFlag, 0);
    InterlockedExchange(&app->busy, 1);
    UpdateButtons(app);
    SetFocus(app->hCancel);

    unsigned (__stdcall *entry)(void *) =
        (stage == STAGE_TRANSCRIBE) ? TranscribeThread :
        (stage == STAGE_BACKEND)    ? EnsureBackendThread : SummarizeThread;

    uintptr_t th = _beginthreadex(NULL, 0, entry, wa, 0, NULL);
    if (th == 0) {
        free(wa->transcript);
        free(wa);
        InterlockedExchange(&app->busy, 0);
        ProgressPct(app, 0);
        SetWindowTextW(app->hStatus, L"Failed to start worker thread.");
        UpdateButtons(app);
    } else {
        app->hWorker = (HANDLE)th;
    }
}

/* --------------------------- commands ----------------------------- */

static void OnTranscribe(AppState *app)
{
    if (app->busy)
        return;
    if (!g_bootDone) {
        MessageBoxW(app->hMain,
                    L"Components are still being prepared. Please wait.",
                    L"YTTranscript", MB_OK | MB_ICONINFORMATION);
        return;
    }

    WCHAR url[2048];
    GetWindowTextW(app->hUrl, url, 2048);

    if (!IsPlausibleYouTubeUrl(url)) {
        MessageBoxW(app->hMain,
                    L"Please enter a valid YouTube URL\n"
                    L"(youtube.com, m.youtube.com or youtu.be).",
                    L"Invalid URL", MB_OK | MB_ICONWARNING);
        SetFocus(app->hUrl);
        return;
    }

    SetWindowTextW(app->hTranscript, L"");
    SetWindowTextW(app->hSummary,    L"");
    g_haveTranscript = FALSE;
    g_haveSummary    = FALSE;
    SetWindowTextW(app->hStatus, L"Starting transcription…");
    ProgressMarquee(app, TRUE);

    StartWorker(app, STAGE_TRANSCRIBE, url, NULL);
}

static void OnSummarize(AppState *app)
{
    if (app->busy)
        return;
    if (!g_haveTranscript) {
        MessageBoxW(app->hMain, L"Transcribe a video first.",
                    L"YTTranscript", MB_OK | MB_ICONINFORMATION);
        return;
    }

    int len = GetWindowTextLengthW(app->hTranscript);
    if (len <= 0) {
        MessageBoxW(app->hMain, L"The transcript is empty.",
                    L"YTTranscript", MB_OK | MB_ICONINFORMATION);
        return;
    }

    WCHAR *buf = (WCHAR *)malloc((size_t)(len + 1) * sizeof(WCHAR));
    if (!buf) {
        SetWindowTextW(app->hStatus, L"Out of memory.");
        return;
    }
    GetWindowTextW(app->hTranscript, buf, len + 1);

    SetWindowTextW(app->hSummary, L"");
    g_haveSummary = FALSE;
    SetWindowTextW(app->hStatus, L"Summarizing…");
    ProgressMarquee(app, TRUE);

    StartWorker(app, STAGE_SUMMARIZE, NULL, buf);
}

static void OnCopy(AppState *app)
{
    int len = GetWindowTextLengthW(app->hSummary);
    if (len <= 0) {
        MessageBoxW(app->hMain, L"There is no summary to copy yet.",
                    L"YTTranscript", MB_OK | MB_ICONINFORMATION);
        return;
    }

    WCHAR *buf = (WCHAR *)malloc((size_t)(len + 1) * sizeof(WCHAR));
    if (!buf)
        return;
    GetWindowTextW(app->hSummary, buf, len + 1);

    CopyTextToClipboard(app->hMain, buf);
    free(buf);

    SetWindowTextW(app->hStatus, L"Summary copied to clipboard.");
}

static void OnCancel(AppState *app)
{
    if (!app->busy)
        return;
    InterlockedExchange(&app->cancelFlag, 1);
    EnterCriticalSection(&app->cs);
    if (app->hChild)
        TerminateProcess(app->hChild, 1);
    LeaveCriticalSection(&app->cs);
    SetWindowTextW(app->hStatus, L"Cancelling…");
}

static void OnEncoderSettings(AppState *app)
{
    if (app->busy) {
        MessageBoxW(app->hMain, L"Please wait for the current task to finish.",
                    L"YTTranscript", MB_OK | MB_ICONINFORMATION);
        return;
    }
    ShowEncoderDialog(app);
    /* If a GPU engine was chosen and its backend isn't here yet, fetch it. */
    if (BackendNeeded(app)) {
        SetWindowTextW(app->hStatus, L"Preparing GPU backend…");
        ProgressMarquee(app, TRUE);
        StartWorker(app, STAGE_BACKEND, NULL, NULL);
    }
}

/* ---------------------------- layout ------------------------------ */

void LayoutChildren(AppState *app)
{
    if (!app || !app->hMain)
        return;

    RECT rc;
    GetClientRect(app->hMain, &rc);

    UINT dpi = GetDpiForWindow(app->hMain);
    if (dpi == 0)
        dpi = 96;
#define SC(x) MulDiv((x), (int)dpi, 96)

    int margin  = SC(12);
    int gap     = SC(8);
    int rh      = SC(28);
    int lblH    = SC(16);
    int statusH = SC(18);
    int progH   = SC(16);
    int bw      = SC(112);

    int W = rc.right  - rc.left;
    int H = rc.bottom - rc.top;

    int fullW = W - 2 * margin;
    if (fullW < SC(220))
        fullW = SC(220);

    int x = margin;
    int y = margin;

    HWND hUrlLabel = GetDlgItem(app->hMain, IDC_URL_LABEL);
    HWND hTrLabel  = GetDlgItem(app->hMain, IDC_TRANSCRIPT_LABEL);
    HWND hSumLabel = GetDlgItem(app->hMain, IDC_SUMMARY_LABEL);

    int fixedTop = lblH + SC(4) + rh + gap + lblH + SC(2);
    int midBlock = gap + statusH + SC(4) + progH + gap + rh + gap + lblH + SC(2);
    int avail = H - 2 * margin - fixedTop - midBlock;
    if (avail < SC(160))
        avail = SC(160);
    int trH  = (avail * 58) / 100;
    int sumH = avail - trH;

    HDWP h = BeginDeferWindowPos(12);

    h = DeferWindowPos(h, hUrlLabel, NULL, x, y, fullW, lblH,
                       SWP_NOZORDER | SWP_NOACTIVATE);
    y += lblH + SC(4);

    int editW = fullW - 2 * (bw + gap);
    if (editW < SC(140))
        editW = SC(140);
    h = DeferWindowPos(h, app->hUrl, NULL, x, y, editW, rh,
                       SWP_NOZORDER | SWP_NOACTIVATE);
    h = DeferWindowPos(h, app->hTranscribe, NULL,
                       x + editW + gap, y, bw, rh,
                       SWP_NOZORDER | SWP_NOACTIVATE);
    h = DeferWindowPos(h, app->hCancel, NULL,
                       x + editW + gap + bw + gap, y, bw, rh,
                       SWP_NOZORDER | SWP_NOACTIVATE);
    y += rh + gap;

    h = DeferWindowPos(h, hTrLabel, NULL, x, y, fullW, lblH,
                       SWP_NOZORDER | SWP_NOACTIVATE);
    y += lblH + SC(2);

    h = DeferWindowPos(h, app->hTranscript, NULL, x, y, fullW, trH,
                       SWP_NOZORDER | SWP_NOACTIVATE);
    y += trH + gap;

    h = DeferWindowPos(h, app->hStatus, NULL, x, y, fullW, statusH,
                       SWP_NOZORDER | SWP_NOACTIVATE);
    y += statusH + SC(4);

    h = DeferWindowPos(h, app->hProgress, NULL, x, y, fullW, progH,
                       SWP_NOZORDER | SWP_NOACTIVATE);
    y += progH + gap;

    h = DeferWindowPos(h, app->hSummarize, NULL, x, y, bw, rh,
                       SWP_NOZORDER | SWP_NOACTIVATE);
    h = DeferWindowPos(h, app->hCopy, NULL, x + bw + gap, y, rh, rh,
                       SWP_NOZORDER | SWP_NOACTIVATE);
    y += rh + gap;

    h = DeferWindowPos(h, hSumLabel, NULL, x, y, fullW, lblH,
                       SWP_NOZORDER | SWP_NOACTIVATE);
    y += lblH + SC(2);

    h = DeferWindowPos(h, app->hSummary, NULL, x, y, fullW, sumH,
                       SWP_NOZORDER | SWP_NOACTIVATE);

    EndDeferWindowPos(h);
#undef SC
}

static void LoadClipIcon(AppState *app, HWND hwnd)
{
    UINT dpi = GetDpiForWindow(hwnd);
    if (dpi == 0) dpi = 96;
    int sz = MulDiv(16, (int)dpi, 96);
    HICON ic = (HICON)LoadImageW(GetModuleHandleW(NULL),
                   MAKEINTRESOURCEW(IDI_CLIPBOARD), IMAGE_ICON, sz, sz,
                   LR_DEFAULTCOLOR);
    if (!ic) return;
    if (app->hClipIcon) DestroyIcon(app->hClipIcon);
    app->hClipIcon = ic;
    if (app->hCopy)
        SendMessageW(app->hCopy, BM_SETIMAGE, IMAGE_ICON, (LPARAM)ic);
}

/* --------------------------- creation ----------------------------- */

static void BuildControls(HWND hwnd, AppState *app, HINSTANCE hInst)
{
    app->hMain     = hwnd;
    app->hUiFont   = CreateUiFont(hwnd);

    CreateWindowExW(0, L"STATIC", L"&YouTube URL:",
                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                    0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_URL_LABEL,
                    hInst, NULL);

    app->hUrl = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                    0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_URL, hInst, NULL);
    SendMessageW(app->hUrl, EM_SETLIMITTEXT, 2040, 0);
    SendMessageW(app->hUrl, EM_SETCUEBANNER, TRUE,
                 (LPARAM)L"https://www.youtube.com/watch?v=…");

    app->hTranscribe = CreateWindowExW(0, L"BUTTON", L"&Transcribe",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                    0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_TRANSCRIBE,
                    hInst, NULL);

    app->hCancel = CreateWindowExW(0, L"BUTTON", L"&Cancel",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                    0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_CANCEL,
                    hInst, NULL);

    CreateWindowExW(0, L"STATIC", L"Transcript",
                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                    0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_TRANSCRIPT_LABEL,
                    hInst, NULL);

    /* No WS_HSCROLL / ES_AUTOHSCROLL: the multiline edit then word-wraps to
     * the control width, so text reads top-to-bottom with only a vertical
     * scrollbar (matches the Summary box). */
    app->hTranscript = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                    0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_TRANSCRIPT,
                    hInst, NULL);

    app->hStatus = CreateWindowExW(0, L"STATIC", L"Preparing components…",
                    WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS,
                    0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_STATUS,
                    hInst, NULL);

    app->hProgress = CreateWindowExW(0, YTT_PROGRESS_CLASS, NULL,
                    WS_CHILD | WS_VISIBLE,
                    0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_PROGRESS,
                    hInst, NULL);
    SendMessageW(app->hProgress, PBM_SETPOS, 0, 0);

    app->hSummarize = CreateWindowExW(0, L"BUTTON", L"Summari&ze",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                    0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SUMMARIZE,
                    hInst, NULL);

    app->hCopy = CreateWindowExW(0, L"BUTTON", L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_ICON,
                    0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_COPY,
                    hInst, NULL);
    LoadClipIcon(app, hwnd);

    CreateWindowExW(0, L"STATIC", L"Summary",
                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                    0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SUMMARY_LABEL,
                    hInst, NULL);

    app->hSummary = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                    0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SUMMARY,
                    hInst, NULL);

    /* Lift the default edit-control character cap so a long transcript or
     * summary is never silently truncated (0 => the multiline maximum). */
    SendMessageW(app->hTranscript, EM_SETLIMITTEXT, 0, 0);
    SendMessageW(app->hSummary,    EM_SETLIMITTEXT, 0, 0);

    /* Tooltip on the icon-only Copy button. */
    HWND tip = CreateWindowExW(0, TOOLTIPS_CLASSW, NULL,
                    WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                    CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                    hwnd, NULL, hInst, NULL);
    if (tip) {
        TOOLINFOW ti;
        ZeroMemory(&ti, sizeof(ti));
        ti.cbSize   = sizeof(ti);
        ti.uFlags   = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd     = hwnd;
        ti.uId      = (UINT_PTR)app->hCopy;
        ti.hinst    = hInst;
        ti.lpszText = (LPWSTR)L"Copy summary to clipboard";
        SendMessageW(tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }

    ApplyFontToAll(app);
    UpdateButtons(app);
    LayoutChildren(app);
}

/* ---------------------------- WndProc ----------------------------- */

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    AppState *app = (AppState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_NCCREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lParam;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          (LONG_PTR)cs->lpCreateParams);
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lParam;
        if (app)
            BuildControls(hwnd, app, cs->hInstance);
        return 0;
    }

    case WM_SETFOCUS:
        if (app) {
            if (!app->busy && IsWindowEnabled(app->hUrl))
                SetFocus(app->hUrl);
            else if (IsWindowEnabled(app->hCancel))
                SetFocus(app->hCancel);
        }
        return 0;

    case WM_SIZE:
        if (app)
            LayoutChildren(app);
        return 0;

    case WM_DPICHANGED: {
        if (app) {
            RECT *r = (RECT *)lParam;
            SetWindowPos(hwnd, NULL, r->left, r->top,
                         r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            if (app->hUiFont)
                DeleteObject(app->hUiFont);
            app->hUiFont = CreateUiFont(hwnd);
            ApplyFontToAll(app);
            LayoutChildren(app);
            LoadClipIcon(app, hwnd);
        }
        return 0;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lParam;
        UINT dpi = GetDpiForWindow(hwnd);
        if (dpi == 0)
            dpi = 96;
        mmi->ptMinTrackSize.x = MulDiv(580, (int)dpi, 96);
        mmi->ptMinTrackSize.y = MulDiv(540, (int)dpi, 96);
        return 0;
    }

    case WM_COMMAND:
        if (app && lParam == 0 && HIWORD(wParam) == 0 &&   /* menu item */
            LOWORD(wParam) == IDM_SETTINGS_ENCODER) {
            OnEncoderSettings(app);
            return 0;
        }
        if (app && HIWORD(wParam) == BN_CLICKED) {
            switch (LOWORD(wParam)) {
            case IDC_TRANSCRIBE: OnTranscribe(app); return 0;
            case IDC_SUMMARIZE:  OnSummarize(app);  return 0;
            case IDC_COPY:       OnCopy(app);       return 0;
            case IDC_CANCEL:     OnCancel(app);     return 0;
            default: break;
            }
        }
        break;

    case WM_CTLCOLORSTATIC:
        if (app) {
            HWND ctl = (HWND)lParam;
            if (ctl == app->hTranscript || ctl == app->hSummary) {
                HDC dc = (HDC)wParam;
                SetBkColor(dc, GetSysColor(COLOR_WINDOW));
                SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
                return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
            }
        }
        break;

    /* ------------------- worker -> UI messages -------------------- */

    case WM_APP_DL_PROGRESS: {
        if (app) {
            int pct = (int)(LONG)wParam;
            int idx = (int)lParam;
            const WCHAR *nm = (idx >= 0 && idx < ASSET_COUNT)
                                  ? kAssetNames[idx] : L"components";
            WCHAR buf[192];
            if (pct < 0) {
                _snwprintf(buf, 192, L"Downloading %ls…", nm);
                ProgressMarquee(app, TRUE);
            } else {
                _snwprintf(buf, 192, L"Downloading %ls… %d%%", nm, pct);
                ProgressPct(app, pct);
            }
            buf[191] = L'\0';
            SetWindowTextW(app->hStatus, buf);
        }
        return 0;
    }

    case WM_APP_DL_DONE: {
        if (app) {
            int idx = (int)wParam;
            const WCHAR *nm = (idx >= 0 && idx < ASSET_COUNT)
                                  ? kAssetNames[idx] : L"component";
            WCHAR buf[192];
            _snwprintf(buf, 192, L"Installed %ls.", nm);
            buf[191] = L'\0';
            SetWindowTextW(app->hStatus, buf);
        }
        return 0;
    }

    case WM_APP_BOOT_DONE:
        if (app) {
            g_bootDone = TRUE;
            ProgressPct(app, 0);
            SetWindowTextW(app->hStatus,
                L"Ready. Paste a YouTube URL and click Transcribe.");
            WorkerFinished(app);
        }
        return 0;

    case WM_APP_STATUS: {
        WCHAR *s = (WCHAR *)lParam;
        if (s) {
            if (app) {
                if (g_opKind != 0) {           /* during an op: this is a phase label */
                    lstrcpynW(g_opLabel, s, ARRAYSIZE(g_opLabel));
                    StripEllipsis(g_opLabel);
                    OpTick(app);
                } else {
                    SetWindowTextW(app->hStatus, s);
                }
            }
            free(s);
        }
        return 0;
    }

    case WM_APP_OPBEGIN:
        if (app)
            OpBegin(app, (int)wParam, (ULONGLONG)(DWORD)lParam);
        return 0;

    case WM_TIMER:
        if (app && wParam == OP_TIMER_ID)
            OpTick(app);
        return 0;

    case WM_APP_PROGRESS:
        if (app) {
            if (g_opKind == 2) {
                /* summarize: the op timer drives the bar from the time estimate */
            } else if ((DWORD)wParam == PROGRESS_MARQUEE) {
                ProgressMarquee(app, TRUE);
            } else {
                int pct = (int)wParam;
                g_opPct = pct;
                ProgressPct(app, pct);
                if (g_opKind == 1) {
                    /* Refine a smoothed total-time estimate (EMA, weight 0.3)
                     * from this sample so the ETA counts down smoothly rather
                     * than jumping each time whisper reports a new percentage. */
                    if (pct >= 3 && pct < 100) {
                        ULONGLONG el  = GetTickCount64() - g_opStartTick;
                        ULONGLONG est = el * 100 / (ULONGLONG)pct;
                        g_opEstMs = (g_opEstMs == 0)
                                      ? est
                                      : (g_opEstMs * 7 + est * 3) / 10;
                    }
                    OpTick(app);
                }
            }
        }
        return 0;

    case WM_APP_TRANSCRIPT: {
        WCHAR *s = (WCHAR *)lParam;
        if (s) {
            if (app) {
                SetWindowTextW(app->hTranscript, s);
                g_haveTranscript = TRUE;
                UpdateButtons(app);
            }
            free(s);
        }
        return 0;
    }

    case WM_APP_SUMMARY: {
        WCHAR *s = (WCHAR *)lParam;
        if (s) {
            if (app) {
                SetWindowTextW(app->hSummary, s);
                g_haveSummary = TRUE;
                UpdateButtons(app);
            }
            free(s);
        }
        return 0;
    }

    case WM_APP_DONE:
        if (app) {
            int stage = (int)wParam;
            ProgressPct(app, 100);
            if (stage == STAGE_TRANSCRIBE) {
                ULONGLONG ms = GetTickCount64() - g_opStartTick;
                int words = TranscriptWordCount(app);
                WCHAR dur[16], buf[176];
                FmtDur(ms, dur, ARRAYSIZE(dur));
                if (words > 0 && ms > 0) {
                    unsigned long t = (unsigned long)((ULONGLONG)words * 10000ull / ms);
                    _snwprintf(buf, ARRAYSIZE(buf),
                        L"Transcription complete — %d words in %s  (%lu.%lu words/sec)",
                        words, dur, t / 10, t % 10);
                } else {
                    _snwprintf(buf, ARRAYSIZE(buf), L"Transcription complete.");
                }
                buf[ARRAYSIZE(buf) - 1] = L'\0';
                SetWindowTextW(app->hStatus, buf);
            } else if (stage == STAGE_SUMMARIZE) {
                /* Real llama tokens/sec (excludes model-load time) so the
                 * engine difference is visible regardless of video length. */
                ULONGLONG ms = GetTickCount64() - g_opStartTick;
                WCHAR dur[16], buf[176];
                FmtDur(ms, dur, ARRAYSIZE(dur));
                if (app->sumGenTps > 0.0) {
                    _snwprintf(buf, ARRAYSIZE(buf),
                        L"Summary complete — %d tok/s generate · %d tok/s prefill  (%s total)",
                        (int)(app->sumGenTps + 0.5), (int)(app->sumPrefillTps + 0.5), dur);
                } else {
                    int words = TranscriptWordCount(app);
                    if (words > 0 && ms > 0) {
                        unsigned long t = (unsigned long)((ULONGLONG)words * 10000ull / ms);
                        _snwprintf(buf, ARRAYSIZE(buf),
                            L"Summary complete — %d words in %s  (%lu.%lu words/sec)",
                            words, dur, t / 10, t % 10);
                    } else {
                        _snwprintf(buf, ARRAYSIZE(buf), L"Summary complete.");
                    }
                }
                buf[ARRAYSIZE(buf) - 1] = L'\0';
                SetWindowTextW(app->hStatus, buf);
            } else if (stage == STAGE_BACKEND) {
                SetWindowTextW(app->hStatus, L"GPU backend ready.");
            } else {
                SetWindowTextW(app->hStatus, L"Done.");
            }
            WorkerFinished(app);
        }
        return 0;

    case WM_APP_ERROR: {
        WCHAR *s = (WCHAR *)lParam;
        if (app) {
            ProgressPct(app, 0);
            if (s) {
                SetWindowTextW(app->hStatus, s);
                MessageBoxW(app->hMain, s, L"YTTranscript — Error",
                            MB_OK | MB_ICONERROR);
            }
            WorkerFinished(app);
        }
        free(s);
        return 0;
    }

    case WM_CLOSE:
        if (app) {
            InterlockedExchange(&app->cancelFlag, 1);
            EnterCriticalSection(&app->cs);
            if (app->hChild)
                TerminateProcess(app->hChild, 1);
            LeaveCriticalSection(&app->cs);
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (app) {
            OpEnd(app);
            InterlockedExchange(&app->cancelFlag, 1);
            EnterCriticalSection(&app->cs);
            if (app->hChild)
                TerminateProcess(app->hChild, 1);
            LeaveCriticalSection(&app->cs);
            if (app->hWorker) {
                WaitForSingleObject(app->hWorker, INFINITE);
                CloseHandle(app->hWorker);
                app->hWorker = NULL;
            }
            if (app->hUiFont) {
                DeleteObject(app->hUiFont);
                app->hUiFont = NULL;
            }
            if (app->hClipIcon) {
                DestroyIcon(app->hClipIcon);
                app->hClipIcon = NULL;
            }
        }
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
