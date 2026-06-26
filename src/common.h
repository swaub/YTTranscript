#ifndef YTT_COMMON_H
#define YTT_COMMON_H

/* ------------------------------------------------------------------ *
 *  YTTranscript.exe  --  shared contract for every translation unit.
 *  Pure C11 / Win32 only.  Compiled by main, ui, util, proc, pipeline,
 *  strutil, summarize, download, bootstrap, clipboard.  This header
 *  emits NO symbols (only macros, types, prototypes) so including it
 *  from every .c never creates duplicate-definition link errors.
 * ------------------------------------------------------------------ */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define WINVER       0x0A00   /* Windows 10/11 */
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <winhttp.h>
#include <stddef.h>

/* =========================== Resource ids ========================= */
#define IDI_APPICON    101
#define IDI_CLIPBOARD  102
#define IDR_MANIFEST     1    /* CREATEPROCESS_MANIFEST_RESOURCE_ID */
#define IDR_MAINMENU         200
#define IDD_ENCODER          300
#define IDM_SETTINGS_ENCODER 3001
#define IDC_HW_INFO          3002
#define IDC_TRANS_COMBO      3003
#define IDC_SUM_COMBO        3004

/* =========================== Control ids ========================== */
enum {
    IDC_URL        = 1001,
    IDC_TRANSCRIBE = 1002,
    IDC_TRANSCRIPT = 1003,
    IDC_STATUS     = 1004,
    IDC_PROGRESS   = 1005,
    IDC_SUMMARIZE  = 1006,
    IDC_SUMMARY    = 1007,
    IDC_COPY       = 1008,
    IDC_CANCEL     = 1009
};

/* ============= Worker -> UI messages (PostMessageW ONLY) ==========
 * Threading contract: workers NEVER touch HWNDs or call SendMessage.
 * Each lParam marked WCHAR* below is heap-allocated by the worker; the
 * UI-thread handler is the SOLE owner and frees it with free().  Always
 * check the PostMessageW return value and free the payload if it fails.
 */
#define WM_APP_DL_PROGRESS (WM_APP + 1) /* wParam = pct 0..100 (-1 unknown), lParam = asset index   */
#define WM_APP_DL_DONE     (WM_APP + 2) /* wParam = asset index that finished                       */
#define WM_APP_BOOT_DONE   (WM_APP + 3) /* wParam = 0; all assets present, enable the UI            */
#define WM_APP_STATUS      (WM_APP + 4) /* lParam = WCHAR* status line                              */
#define WM_APP_PROGRESS    (WM_APP + 5) /* wParam = pct 0..100, or 0xFFFFFFFFu = marquee            */
#define WM_APP_TRANSCRIPT  (WM_APP + 6) /* lParam = WCHAR* cleaned transcript (UI sets edit, frees) */
#define WM_APP_SUMMARY     (WM_APP + 7) /* lParam = WCHAR* final summary (UI sets edit, frees)      */
#define WM_APP_DONE        (WM_APP + 8) /* wParam = stage id; one pipeline finished OK             */
#define WM_APP_ERROR       (WM_APP + 9) /* lParam = WCHAR* error message (UI shows, frees)          */
#define WM_APP_OPBEGIN     (WM_APP +10) /* wParam = 1 transcribe / 2 summarize; lParam = est total ms */

#define PROGRESS_MARQUEE   0xFFFFFFFFu

/* Stage ids carried by WM_APP_DONE and selecting the worker entry. */
enum { STAGE_BOOTSTRAP = 1, STAGE_TRANSCRIBE = 2, STAGE_SUMMARIZE = 3,
       STAGE_BACKEND = 4 };

/* First-run asset table.  Order MUST match g_assets[] in bootstrap.c. */
enum { ASSET_YTDLP, ASSET_FFMPEG, ASSET_WHISPER, ASSET_WMODEL,
       ASSET_LLAMA, ASSET_LMODEL, ASSET_COUNT };

/* =========================== Hardware ============================= */
enum { GPU_VENDOR_OTHER, GPU_VENDOR_NVIDIA, GPU_VENDOR_AMD, GPU_VENDOR_INTEL };

typedef struct {
    WCHAR     name[128];
    int       vendor;       /* GPU_VENDOR_*                              */
    BOOL      isDiscrete;   /* dedicated VRAM over the iGPU threshold    */
    ULONGLONG vram;         /* dedicated video memory, bytes            */
} GpuInfo;

typedef struct {
    GpuInfo gpus[8];
    int     count;
    BOOL    hasNvidia;      /* => CUDA can be offered                    */
    BOOL    hasAnyGpu;      /* => Vulkan can be offered                  */
} HwInfo;

/* Engine ids persisted in settings.cfg and used to route subprocesses. */
enum { ENGINE_CPU, ENGINE_CUDA, ENGINE_VULKAN };

/* =========================== Shared types ========================= */

/* All paths resolved from GetModuleFileNameW, never the CWD. */
typedef struct {
    WCHAR exeDir [MAX_PATH];  /* folder holding YTTranscript.exe         */
    WCHAR bin    [MAX_PATH];  /* exeDir\\bin                              */
    WCHAR whisper[MAX_PATH];  /* exeDir\\bin\\whisper  (own ggml DLLs)    */
    WCHAR llama  [MAX_PATH];  /* exeDir\\bin\\llama    (own ggml DLLs)    */
    WCHAR models [MAX_PATH];  /* exeDir\\models                          */
    WCHAR temp   [MAX_PATH];  /* exeDir\\temp   (m4a/wav/_prompt.txt)     */
    WCHAR output [MAX_PATH];  /* exeDir\\output (<id>.txt transcript)     */
} AppPaths;

/* One first-run download manifest entry. */
typedef struct {
    LPCWSTR url;         /* https source                                  */
    LPCWSTR archive;     /* temp zip name, or NULL for a direct file      */
    LPCWSTR dest;        /* final file (relative to exeDir) to test/skip  */
    LPCWSTR extractDir;  /* dir to `tar -xf` into, or NULL for direct dl  */
    LPCWSTR key;         /* stable manifest key recorded in installed.cfg */
    LPCWSTR version;     /* pinned version tag; L"" => yt-dlp online check */
    DWORD   minBytes;    /* integrity floor: a smaller dest is "broken"   */
} AssetSpec;

/* Result of a captured subprocess run. */
typedef struct {
    DWORD  exitCode;
    char  *out;     /* malloc'd UTF-8 stdout (may be NULL), NUL-terminated */
    size_t outLen;
} ProcResult;

/* stderr line callback (one UTF-8 line at a time) used for progress. */
typedef void (*LineCb)(void *ud, const char *utf8_line);

/* Single application-state object (one per top-level window). */
typedef struct {
    HWND  hMain;
    HWND  hUrl, hTranscribe, hTranscript, hStatus, hProgress;
    HWND  hSummarize, hSummary, hCopy, hCancel;
    HFONT hUiFont;
    HICON hClipIcon;
    AppPaths paths;
    /* worker / cancellation */
    HANDLE           hWorker;     /* current worker thread, or NULL        */
    HANDLE           hChild;      /* live child process, guarded by cs     */
    CRITICAL_SECTION cs;          /* guards hChild + cancelFlag             */
    volatile LONG    cancelFlag;  /* set by Cancel, polled by the worker    */
    volatile LONG    busy;        /* 1 while any worker runs                */
    /* encoder selection + detected graphics hardware */
    int    engTranscribe;         /* ENGINE_CPU | ENGINE_CUDA               */
    int    engSummarize;          /* ENGINE_CPU | ENGINE_VULKAN | ENGINE_CUDA */
    HwInfo hw;
    double sumPrefillTps;         /* last summarize: prompt-eval tokens/sec  */
    double sumGenTps;             /* last summarize: generation tokens/sec   */
} AppState;

/* Argument bundle handed to a worker thread (heap; the worker frees it). */
typedef struct {
    HWND      hwnd;
    AppState *app;
    int       stage;           /* STAGE_*                                  */
    WCHAR     url[2048];       /* validated YouTube URL (transcribe)       */
    WCHAR    *transcript;      /* heap UTF-16 transcript (summarize)       */
} WorkerArgs;

/* =========================== progressbar.c ====================== */
#define YTT_PROGRESS_CLASS L"YTTProgressBar"
void   RegisterProgressBarClass(HINSTANCE hInst);

/* =========================== hwdetect.c ========================== */
void   DetectHardware(HwInfo *out);   /* DXGI adapter enumeration */

/* =========================== settings.c ========================== */
void   LoadSettings(AppState *app);   /* read settings.cfg, validate vs hw */
void   SaveSettings(AppState *app);
void   ShowEncoderDialog(AppState *app);  /* modal; may queue a backend download */

/* paths (relative to exeDir) of the backend executables, per engine. */
void   WhisperExeFor(const AppState *app, WCHAR *exe, WCHAR *dir);
void   LlamaExeFor(const AppState *app, WCHAR *exe, WCHAR *dir, int *ngl);

/* =========================== bootstrap.c (backends) ============== */
BOOL   BackendNeeded(const AppState *app);            /* a selected GPU backend is missing */
unsigned __stdcall EnsureBackendThread(void *args);   /* WorkerArgs* downloads it */

/* ============================ util.c ============================== */
void   ResolvePaths(AppPaths *p);                   /* fill + CreateDirectory */
BOOL   IsPlausibleYouTubeUrl(const WCHAR *u);       /* security gate          */
void   PathUnder(const AppPaths *p, WCHAR *out, const WCHAR *rel);
char  *WideToUtf8(const WCHAR *w);                  /* malloc'd; free()       */
WCHAR *Utf8ToWide(const char *s);                   /* malloc'd; free()       */
char  *ReadFileUtf8(const WCHAR *path);             /* malloc'd; strips BOM   */
BOOL   WriteFileUtf8(const WCHAR *path, const char *utf8); /* no BOM          */
BOOL   FileExists(const WCHAR *path);
int    CpuThreads(void);                            /* clamped to [1,16]      */

/* =========================== proc.c ============================== */
void   ArgAppend(WCHAR *dst, size_t cap, const WCHAR *arg); /* dst starts L"" */
BOOL   RunProcess(WCHAR *cmdline, const WCHAR *workdir,
                  LineCb on_stderr, void *ud, ProcResult *res);

/* ============================ ui.c =============================== */
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void   LayoutChildren(AppState *app);               /* DPI-scaled layout      */

/* ========================= clipboard.c ========================== */
void   CopyTextToClipboard(HWND owner, const WCHAR *text); /* CF_UNICODETEXT  */

/* ========================== pipeline.c ========================== */
unsigned __stdcall TranscribeThread(void *args);    /* WorkerArgs*            */

/* ========================== strutil.c =========================== */
char  *CleanTranscript(const char *utf8_in);        /* malloc'd UTF-8         */
char **ChunkText(const char *utf8, size_t maxChars,
                 size_t overlap, size_t *count);     /* malloc'd array+items   */

/* ========================= summarize.c ========================== */
unsigned __stdcall SummarizeThread(void *args);     /* WorkerArgs*            */
char  *SummarizeTranscript(AppState *app, HWND ui, const char *utf8_transcript);

/* ========================== download.c ========================== */
BOOL   DownloadFile(HWND hwnd, int assetIndex, LPCWSTR url, LPCWSTR destPath);

/* ========================== bootstrap.c ========================= */
unsigned __stdcall BootstrapThread(void *args);     /* WorkerArgs*            */
BOOL   AssetsPresent(const AppPaths *p);

#endif /* YTT_COMMON_H */