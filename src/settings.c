/* ------------------------------------------------------------------ *
 *  settings.c -- encoder selection: persistence (settings.cfg), the
 *  CPU/Vulkan/CUDA routing helpers, and the modal Encoder dialog.
 * ------------------------------------------------------------------ */

#include "common.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ----------------------------- config ---------------------------- */

static const char *EngStr(int e)
{
    return (e == ENGINE_CUDA)   ? "cuda" :
           (e == ENGINE_VULKAN) ? "vulkan" : "cpu";
}

static int EngFromStr(const char *s)
{
    if (s && _stricmp(s, "cuda")   == 0) return ENGINE_CUDA;
    if (s && _stricmp(s, "vulkan") == 0) return ENGINE_VULKAN;
    return ENGINE_CPU;
}

static void CfgPath(const AppState *app, WCHAR *out)
{
    PathCombineW(out, app->paths.exeDir, L"settings.cfg");
}

/* A selection is only honored if the hardware actually supports it. */
static int ValidateEngine(const AppState *app, int e, BOOL allowVulkan)
{
    if (e == ENGINE_CUDA   && !app->hw.hasNvidia) return ENGINE_CPU;
    if (e == ENGINE_VULKAN && !(allowVulkan && app->hw.hasAnyGpu)) return ENGINE_CPU;
    return e;
}

void LoadSettings(AppState *app)
{
    app->engTranscribe = ENGINE_CPU;
    app->engSummarize  = ENGINE_CPU;

    WCHAR p[MAX_PATH];
    CfgPath(app, p);
    char *buf = ReadFileUtf8(p);
    if (buf) {
        char *line = buf;
        while (*line) {
            char *nl = line;
            while (*nl && *nl != '\n' && *nl != '\r') nl++;
            char saved = *nl; *nl = '\0';
            char *eq = strchr(line, '=');
            if (eq) {
                *eq = '\0';
                const char *k = line, *v = eq + 1;
                if (_stricmp(k, "transcribe") == 0) app->engTranscribe = EngFromStr(v);
                else if (_stricmp(k, "summarize") == 0) app->engSummarize = EngFromStr(v);
            }
            if (saved == '\0') break;
            line = nl + 1;
        }
        free(buf);
    }

    /* whisper has no Vulkan build, so transcribe is only CPU or CUDA. */
    app->engTranscribe = ValidateEngine(app, app->engTranscribe, FALSE);
    app->engSummarize  = ValidateEngine(app, app->engSummarize,  TRUE);
}

void SaveSettings(AppState *app)
{
    char buf[128];
    _snprintf(buf, sizeof buf, "transcribe=%s\r\nsummarize=%s\r\n",
              EngStr(app->engTranscribe), EngStr(app->engSummarize));
    buf[sizeof buf - 1] = '\0';
    WCHAR p[MAX_PATH];
    CfgPath(app, p);
    WriteFileUtf8(p, buf);
}

/* ----------------------------- routing --------------------------- */

void WhisperExeFor(const AppState *app, WCHAR *exe, WCHAR *dir)
{
    if (app->engTranscribe == ENGINE_CUDA) {
        _snwprintf(dir, MAX_PATH, L"%s\\whisper-cuda", app->paths.bin);
        _snwprintf(exe, MAX_PATH, L"%s\\whisper-cuda\\whisper-cli.exe", app->paths.bin);
        dir[MAX_PATH-1] = exe[MAX_PATH-1] = L'\0';
        if (FileExists(exe)) return;          /* else fall back to CPU below */
    }
    _snwprintf(dir, MAX_PATH, L"%s\\whisper", app->paths.bin);
    _snwprintf(exe, MAX_PATH, L"%s\\whisper\\whisper-cli.exe", app->paths.bin);
    dir[MAX_PATH-1] = exe[MAX_PATH-1] = L'\0';
}

void LlamaExeFor(const AppState *app, WCHAR *exe, WCHAR *dir, int *ngl)
{
    const WCHAR *sub = NULL;
    if (app->engSummarize == ENGINE_CUDA)        sub = L"llama-cuda";
    else if (app->engSummarize == ENGINE_VULKAN) sub = L"llama-vulkan";

    if (sub) {
        _snwprintf(dir, MAX_PATH, L"%s\\%s", app->paths.bin, sub);
        _snwprintf(exe, MAX_PATH, L"%s\\%s\\llama-completion.exe", app->paths.bin, sub);
        dir[MAX_PATH-1] = exe[MAX_PATH-1] = L'\0';
        if (FileExists(exe)) { *ngl = 99; return; }   /* offload all layers */
    }
    _snwprintf(dir, MAX_PATH, L"%s\\llama", app->paths.bin);
    _snwprintf(exe, MAX_PATH, L"%s\\llama\\llama-completion.exe", app->paths.bin);
    dir[MAX_PATH-1] = exe[MAX_PATH-1] = L'\0';
    *ngl = 0;
}

/* --------------------------- the dialog -------------------------- */

static void FillCombo(HWND combo, const AppState *app, BOOL withVulkan, int current)
{
    int sel = 0, idx = 0;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);

    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"CPU");
    SendMessageW(combo, CB_SETITEMDATA, idx, ENGINE_CPU);
    if (current == ENGINE_CPU) sel = idx;
    idx++;

    if (withVulkan && app->hw.hasAnyGpu) {
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"GPU — Vulkan");
        SendMessageW(combo, CB_SETITEMDATA, idx, ENGINE_VULKAN);
        if (current == ENGINE_VULKAN) sel = idx;
        idx++;
    }
    if (app->hw.hasNvidia) {
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"NVIDIA GPU — CUDA");
        SendMessageW(combo, CB_SETITEMDATA, idx, ENGINE_CUDA);
        if (current == ENGINE_CUDA) sel = idx;
        idx++;
    }
    SendMessageW(combo, CB_SETCURSEL, sel, 0);
}

static int ComboEngine(HWND combo)
{
    int i = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (i < 0) return ENGINE_CPU;
    return (int)SendMessageW(combo, CB_GETITEMDATA, i, 0);
}

static INT_PTR CALLBACK EncoderDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    AppState *app = (AppState *)GetWindowLongPtrW(dlg, GWLP_USERDATA);

    switch (msg) {
    case WM_INITDIALOG: {
        app = (AppState *)lp;
        SetWindowLongPtrW(dlg, GWLP_USERDATA, lp);

        WCHAR info[512];
        int n = _snwprintf(info, 512, L"CPU: always available");
        for (int i = 0; i < app->hw.count && n < 460; i++) {
            const WCHAR *v = app->hw.gpus[i].vendor == GPU_VENDOR_NVIDIA ? L"NVIDIA" :
                             app->hw.gpus[i].vendor == GPU_VENDOR_AMD    ? L"AMD"    :
                             app->hw.gpus[i].vendor == GPU_VENDOR_INTEL  ? L"Intel"  : L"GPU";
            n += _snwprintf(info + n, 512 - n, L"\r\n%s: %s (%s)", v, app->hw.gpus[i].name,
                            app->hw.gpus[i].isDiscrete ? L"discrete" : L"integrated");
        }
        info[511] = L'\0';
        SetDlgItemTextW(dlg, IDC_HW_INFO, info);

        FillCombo(GetDlgItem(dlg, IDC_TRANS_COMBO), app, FALSE, app->engTranscribe);
        FillCombo(GetDlgItem(dlg, IDC_SUM_COMBO),   app, TRUE,  app->engSummarize);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            if (app) {
                app->engTranscribe = ComboEngine(GetDlgItem(dlg, IDC_TRANS_COMBO));
                app->engSummarize  = ComboEngine(GetDlgItem(dlg, IDC_SUM_COMBO));
                SaveSettings(app);
            }
            EndDialog(dlg, 1);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

void ShowEncoderDialog(AppState *app)
{
    DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_ENCODER),
                    app->hMain, EncoderDlgProc, (LPARAM)app);
}
