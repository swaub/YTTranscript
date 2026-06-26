/* ------------------------------------------------------------------ *
 *  main.c  --  wWinMain entry point for YTTranscript.exe.
 *
 *  Responsibilities (per spec):
 *    - SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2) BEFORE any
 *      window is created.
 *    - InitCommonControlsEx for v6 controls (progress bar / bars).
 *    - Register the main WNDCLASSEXW with hIcon/hIconSm = IDI_APPICON.
 *    - Allocate + zero exactly one AppState, init its CRITICAL_SECTION,
 *      ResolvePaths().
 *    - Create the main window (passing AppState* as lpCreateParams so
 *      ui.c's WM_NCCREATE can stash it in GWLP_USERDATA).
 *    - Kick off the first-run BootstrapThread via _beginthreadex.
 *    - Run a GetMessageW/TranslateMessage/DispatchMessageW loop
 *      (with IsDialogMessageW so Tab/Enter navigate the controls).
 * ------------------------------------------------------------------ */

#include "common.h"
#include <process.h>
#include <stdlib.h>
#include <wchar.h>

static const WCHAR kClassName[] = L"YTTranscriptMainWindow";

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                    PWSTR lpCmdLine, int nCmdShow)
{
    (void)hPrevInstance;
    (void)lpCmdLine;

    /* Child processes inherit this error mode: a failed/blocked helper (e.g.
     * a "Bad Image"/missing-DLL load) fails cleanly with an exit code instead
     * of popping a modal system dialog that would block our worker threads. */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    /* Per-monitor v2 DPI awareness must be established before the first
     * HWND exists; the manifest declares the same, this is the runtime
     * belt-and-suspenders the spec requires. */
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);
    RegisterProgressBarClass(hInstance);
    LoadLibraryW(L"Msftedit.dll");   /* registers the RICHEDIT50W class for the summary box */

    AppState *app = (AppState *)calloc(1, sizeof(AppState));
    if (!app)
        return 1;
    InitializeCriticalSection(&app->cs);
    ResolvePaths(&app->paths);

    /* Redirect child-process temp out of %LOCALAPPDATA%\Temp into our own
     * (allowed) folder.  The standalone yt-dlp.exe is a PyInstaller bundle
     * that unpacks an unsigned Python runtime (ucrtbase.dll, python310.dll)
     * to %TEMP%\_MEI... and LoadLibrary's it; Smart App Control / WDAC
     * policies that block unsigned DLLs loaded from Temp otherwise kill it
     * ("An Application Control policy has blocked this file").  GetTempPath
     * honors TMP then TEMP, and CreateProcessW children inherit our env. */
    SetEnvironmentVariableW(L"TMP",  app->paths.temp);
    SetEnvironmentVariableW(L"TEMP", app->paths.temp);

    /* Enumerate graphics hardware and load the saved Encoder selection
     * (validated against what this machine actually supports). */
    DetectHardware(&app->hw);
    LoadSettings(app);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hIcon         = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON),
                                         IMAGE_ICON, 0, 0,
                                         LR_DEFAULTSIZE | LR_SHARED);
    wc.hIconSm       = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON),
                                         IMAGE_ICON, 16, 16,
                                         LR_SHARED);
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszMenuName  = MAKEINTRESOURCEW(IDR_MAINMENU);
    wc.lpszClassName = kClassName;

    if (!RegisterClassExW(&wc)) {
        DeleteCriticalSection(&app->cs);
        free(app);
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        0,
        kClassName,
        L"YTTranscript — YouTube Transcribe & Summarize",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 920, 740,
        NULL, NULL, hInstance, app);
    if (!hwnd) {
        DeleteCriticalSection(&app->cs);
        free(app);
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    /* First-run bootstrap: download/extract any missing helper binaries
     * and models on a worker thread so the UI never blocks.  The worker
     * owns and frees the WorkerArgs. */
    WorkerArgs *wa = (WorkerArgs *)calloc(1, sizeof(WorkerArgs));
    if (wa) {
        wa->hwnd  = hwnd;
        wa->app   = app;
        wa->stage = STAGE_BOOTSTRAP;
        InterlockedExchange(&app->busy, 1);
        uintptr_t th = _beginthreadex(NULL, 0, BootstrapThread, wa, 0, NULL);
        if (th == 0) {
            free(wa);
            InterlockedExchange(&app->busy, 0);
            /* Surface it instead of leaving the UI stuck on "Preparing…". */
            WCHAR *err = _wcsdup(L"Could not start first-run setup. "
                                 L"Please relaunch the application.");
            if (err && !PostMessageW(hwnd, WM_APP_ERROR, 0, (LPARAM)err))
                free(err);
        } else {
            app->hWorker = (HANDLE)th;
        }
    }

    MSG msg;
    ZeroMemory(&msg, sizeof(msg));
    BOOL got;
    while ((got = GetMessageW(&msg, NULL, 0, 0)) != 0) {
        if (got == -1)
            break;
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    /* WndProc's WM_DESTROY has already cancelled + joined any worker, so
     * no thread can still reference *app. */
    DeleteCriticalSection(&app->cs);
    free(app);
    return (int)msg.wParam;
}
