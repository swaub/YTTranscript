/* ------------------------------------------------------------------ *
 *  pipeline.c -- TranscribeThread worker.
 *
 *  yt-dlp (audio download) -> ffmpeg (16 kHz mono s16 WAV) -> whisper-cli
 *  (-> output\<id>.txt).  Every command line is built as a real argv via
 *  ArgAppend (never a shell), the URL is passed after `--` so it cannot
 *  inject options.  Progress is parsed off each child's stderr and posted
 *  to the UI; the cleaned transcript is posted as an owned WCHAR* that the
 *  UI handler frees.  Cancellation is honored between stages.
 * ------------------------------------------------------------------ */

#include "common.h"
#include <process.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>

extern AppState *g_proc_cancel_app;

/* ------------------------- small UI posters ---------------------- */

static void postStatus(HWND h, const WCHAR *s)
{
    WCHAR *d = _wcsdup(s);
    if (d && !PostMessageW(h, WM_APP_STATUS, 0, (LPARAM)d)) free(d);
}

static void postError(HWND h, const WCHAR *s)
{
    WCHAR *d = _wcsdup(s);
    if (d && !PostMessageW(h, WM_APP_ERROR, 0, (LPARAM)d)) free(d);
}

static void postErrorF(HWND h, const WCHAR *base, DWORD code, const char *last)
{
    WCHAR buf[1200];
    WCHAR *lw = (last && *last) ? Utf8ToWide(last) : NULL;
    if (lw && *lw)
        _snwprintf(buf, 1200, L"%s (exit code %lu): %s", base, (unsigned long)code, lw);
    else
        _snwprintf(buf, 1200, L"%s (exit code %lu).", base, (unsigned long)code);
    buf[1199] = L'\0';
    free(lw);
    postError(h, buf);
}

static void postMarquee(HWND h)
{
    PostMessageW(h, WM_APP_PROGRESS, (WPARAM)PROGRESS_MARQUEE, 0);
}

/* ------------------------- progress parsing ---------------------- */

typedef struct {
    HWND hwnd;
    int  kind;       /* 0 = yt-dlp, 1 = whisper, 2 = plain capture */
    char last[512];
} ProgCtx;

static int parse_pct_before(const char *line, const char *pc)
{
    const char *q = pc;
    while (q > line && (((unsigned char)q[-1] >= '0' && (unsigned char)q[-1] <= '9') ||
                        q[-1] == '.'))
        --q;
    if (q == pc) return -1;
    double v = atof(q);
    if (v < 0.0 || v > 100.0) return -1;
    return (int)(v + 0.5);
}

static int parse_int_skip(const char *s)
{
    while (*s == ' ' || *s == '\t') ++s;
    if (!(*s >= '0' && *s <= '9')) return -1;
    return atoi(s);
}

static void prog_cb(void *ud, const char *line)
{
    ProgCtx *c = (ProgCtx *)ud;
    if (!line) return;

    strncpy(c->last, line, sizeof(c->last) - 1);
    c->last[sizeof(c->last) - 1] = '\0';

    int pct = -1;
    if (c->kind == 0) {
        if (strstr(line, "[download]")) {
            const char *pc = strrchr(line, '%');
            if (pc) pct = parse_pct_before(line, pc);
        }
    } else if (c->kind == 1) {
        const char *p = strstr(line, "progress");
        if (p) {
            const char *eq = strchr(p, '=');
            const char *pc = strchr(p, '%');
            if (eq && pc && pc > eq) pct = parse_int_skip(eq + 1);
        }
    }

    if (pct >= 0 && pct <= 100)
        PostMessageW(c->hwnd, WM_APP_PROGRESS, (WPARAM)pct, 0);
}

/* From yt-dlp's captured stdout, pick the printed file path: the last
 * non-empty line, preferring one that lives under the temp directory. */
static WCHAR *pick_path(const char *utf8out, const WCHAR *tempDir)
{
    WCHAR *w = utf8out ? Utf8ToWide(utf8out) : NULL;
    if (!w) return NULL;

    WCHAR *best = NULL, *lastAny = NULL;
    WCHAR *s = w;
    while (*s) {
        WCHAR *e = s;
        while (*e && *e != L'\n' && *e != L'\r') ++e;
        WCHAR saved = *e;
        *e = L'\0';

        WCHAR *ls = s;
        while (*ls == L' ' || *ls == L'\t') ++ls;
        size_t n = wcslen(ls);
        while (n > 0 && (ls[n - 1] == L' ' || ls[n - 1] == L'\t')) ls[--n] = L'\0';

        if (n > 0) {
            free(lastAny);
            lastAny = _wcsdup(ls);
            if (StrStrIW(ls, tempDir)) {
                free(best);
                best = _wcsdup(ls);
            }
        }
        if (saved == 0) break;
        s = e + 1;
    }
    free(w);

    if (best) { free(lastAny); return best; }
    return lastAny;
}

/* --------------------------- the worker -------------------------- */

unsigned __stdcall TranscribeThread(void *argp)
{
    WorkerArgs *a = (WorkerArgs *)argp;
    AppState *app = a->app;
    HWND h = a->hwnd;
    WCHAR *audioW = NULL;

    g_proc_cancel_app = app;

    WCHAR cmd[8192];
    WCHAR id[MAX_PATH];

    /* ---------------- stage 1: yt-dlp -------------------------- */
    postStatus(h, L"Downloading audio…");
    postMarquee(h);

    {
        WCHAR ytexe[MAX_PATH], ffloc[MAX_PATH], outtpl[MAX_PATH];
        _snwprintf(ytexe, MAX_PATH, L"%s\\yt-dlp.exe", app->paths.bin);
        _snwprintf(ffloc, MAX_PATH, L"%s\\ffmpeg.exe", app->paths.bin);
        _snwprintf(outtpl, MAX_PATH, L"%s\\%%(id)s.%%(ext)s", app->paths.temp);
        ytexe[MAX_PATH - 1] = ffloc[MAX_PATH - 1] = outtpl[MAX_PATH - 1] = L'\0';

        cmd[0] = L'\0';
        ArgAppend(cmd, 8192, ytexe);
        ArgAppend(cmd, 8192, L"-f");                ArgAppend(cmd, 8192, L"bestaudio/best");
        ArgAppend(cmd, 8192, L"--no-playlist");
        ArgAppend(cmd, 8192, L"--no-part");
        ArgAppend(cmd, 8192, L"--no-continue");
        ArgAppend(cmd, 8192, L"--force-overwrites");
        ArgAppend(cmd, 8192, L"--newline");
        ArgAppend(cmd, 8192, L"--ffmpeg-location"); ArgAppend(cmd, 8192, ffloc);
        ArgAppend(cmd, 8192, L"-o");                ArgAppend(cmd, 8192, outtpl);
        ArgAppend(cmd, 8192, L"--print");           ArgAppend(cmd, 8192, L"after_move:filepath");
        ArgAppend(cmd, 8192, L"--");
        ArgAppend(cmd, 8192, a->url);

        ProgCtx ctx; ZeroMemory(&ctx, sizeof ctx); ctx.hwnd = h; ctx.kind = 0;
        ProcResult pr; ZeroMemory(&pr, sizeof pr);

        if (!RunProcess(cmd, app->paths.bin, prog_cb, &ctx, &pr)) {
            postError(h, L"Failed to start yt-dlp.exe.");
            goto cleanup;
        }
        if (app->cancelFlag) { free(pr.out); goto cancelled; }
        if (pr.exitCode != 0) {
            postErrorF(h, L"yt-dlp failed", pr.exitCode, ctx.last);
            free(pr.out);
            goto cleanup;
        }

        audioW = pick_path(pr.out, app->paths.temp);
        free(pr.out);
        if (!audioW || !FileExists(audioW)) {
            postError(h, L"Could not locate the downloaded audio file.");
            goto cleanup;
        }

        const WCHAR *fn = PathFindFileNameW(audioW);
        wcsncpy(id, fn, MAX_PATH - 1);
        id[MAX_PATH - 1] = L'\0';
        WCHAR *dot = wcsrchr(id, L'.');
        if (dot) *dot = L'\0';
    }

    if (app->cancelFlag) goto cancelled;

    /* ---------------- stage 2: ffmpeg -------------------------- */
    postStatus(h, L"Converting audio…");
    postMarquee(h);

    WCHAR wav[MAX_PATH];
    {
        WCHAR ffexe[MAX_PATH];
        _snwprintf(ffexe, MAX_PATH, L"%s\\ffmpeg.exe", app->paths.bin);
        _snwprintf(wav,   MAX_PATH, L"%s\\%s.wav", app->paths.temp, id);
        ffexe[MAX_PATH - 1] = wav[MAX_PATH - 1] = L'\0';

        cmd[0] = L'\0';
        ArgAppend(cmd, 8192, ffexe);
        ArgAppend(cmd, 8192, L"-nostdin");
        ArgAppend(cmd, 8192, L"-hide_banner");
        ArgAppend(cmd, 8192, L"-y");
        ArgAppend(cmd, 8192, L"-loglevel"); ArgAppend(cmd, 8192, L"error");
        ArgAppend(cmd, 8192, L"-i");        ArgAppend(cmd, 8192, audioW);
        ArgAppend(cmd, 8192, L"-ar");       ArgAppend(cmd, 8192, L"16000");
        ArgAppend(cmd, 8192, L"-ac");       ArgAppend(cmd, 8192, L"1");
        ArgAppend(cmd, 8192, L"-c:a");      ArgAppend(cmd, 8192, L"pcm_s16le");
        ArgAppend(cmd, 8192, L"-f");        ArgAppend(cmd, 8192, L"wav");
        ArgAppend(cmd, 8192, wav);

        ProgCtx fctx; ZeroMemory(&fctx, sizeof fctx); fctx.hwnd = h; fctx.kind = 2;
        ProcResult pr; ZeroMemory(&pr, sizeof pr);

        if (!RunProcess(cmd, app->paths.bin, prog_cb, &fctx, &pr)) {
            postError(h, L"Failed to start ffmpeg.exe.");
            goto cleanup;
        }
        free(pr.out);
        if (app->cancelFlag) goto cancelled;
        if (pr.exitCode != 0 || !FileExists(wav)) {
            postErrorF(h, L"ffmpeg failed", pr.exitCode, fctx.last);
            goto cleanup;
        }
    }

    if (app->cancelFlag) goto cancelled;

    /* ---------------- stage 3: whisper-cli --------------------- */
    /* Start the determinate progress + ETA clock; whisper's parsed
     * "progress = N%" then drives the bar and the time-remaining estimate. */
    PostMessageW(h, WM_APP_OPBEGIN, (WPARAM)1, 0);

    WCHAR txtpath[MAX_PATH];
    {
        WCHAR wexe[MAX_PATH], wdir[MAX_PATH], model[MAX_PATH], ofbase[MAX_PATH], tnum[16];
        /* CPU or CUDA whisper, per the Encoder setting (falls back to CPU if
         * the cuBLAS build isn't downloaded). The cuBLAS build auto-uses GPU. */
        WhisperExeFor(app, wexe, wdir);
        _snwprintf(model,  MAX_PATH, L"%s\\ggml-base.en.bin", app->paths.models);
        _snwprintf(ofbase, MAX_PATH, L"%s\\%s", app->paths.output, id);
        _snwprintf(txtpath,MAX_PATH, L"%s\\%s.txt", app->paths.output, id);
        _snwprintf(tnum, 16, L"%d", CpuThreads());
        wexe[MAX_PATH-1]=wdir[MAX_PATH-1]=model[MAX_PATH-1]=ofbase[MAX_PATH-1]=txtpath[MAX_PATH-1]=L'\0';

        cmd[0] = L'\0';
        ArgAppend(cmd, 8192, wexe);
        ArgAppend(cmd, 8192, L"-m");  ArgAppend(cmd, 8192, model);
        ArgAppend(cmd, 8192, L"-f");  ArgAppend(cmd, 8192, wav);
        ArgAppend(cmd, 8192, L"-l");  ArgAppend(cmd, 8192, L"en");
        ArgAppend(cmd, 8192, L"-t");  ArgAppend(cmd, 8192, tnum);
        ArgAppend(cmd, 8192, L"-bs"); ArgAppend(cmd, 8192, L"5");
        ArgAppend(cmd, 8192, L"-np");
        ArgAppend(cmd, 8192, L"-pp");
        ArgAppend(cmd, 8192, L"-otxt");
        ArgAppend(cmd, 8192, L"-of"); ArgAppend(cmd, 8192, ofbase);

        ProgCtx wctx; ZeroMemory(&wctx, sizeof wctx); wctx.hwnd = h; wctx.kind = 1;
        ProcResult pr; ZeroMemory(&pr, sizeof pr);

        if (!RunProcess(cmd, wdir, prog_cb, &wctx, &pr)) {
            postError(h, L"Failed to start whisper-cli.exe.");
            goto cleanup;
        }
        free(pr.out);
        if (app->cancelFlag) goto cancelled;
        if (pr.exitCode != 0) {
            postErrorF(h, L"whisper-cli failed", pr.exitCode, wctx.last);
            goto cleanup;
        }
    }

    if (app->cancelFlag) goto cancelled;

    /* ---------------- read + clean + deliver ------------------- */
    {
        char *raw = ReadFileUtf8(txtpath);
        if (!raw) {
            postError(h, L"The transcript file was not produced.");
            goto cleanup;
        }
        char *clean = CleanTranscript(raw);
        free(raw);
        if (!clean) {
            postError(h, L"Out of memory while cleaning the transcript.");
            goto cleanup;
        }
        WriteFileUtf8(txtpath, clean);   /* persist the cleaned version */

        WCHAR *tw = Utf8ToWide(clean);
        free(clean);

        /* temp media no longer needed */
        DeleteFileW(wav);
        DeleteFileW(audioW);
        free(audioW);
        audioW = NULL;

        if (!tw) {
            postError(h, L"Out of memory while preparing the transcript.");
            goto cleanup;
        }
        if (!PostMessageW(h, WM_APP_TRANSCRIPT, 0, (LPARAM)tw)) free(tw);

        PostMessageW(h, WM_APP_PROGRESS, 100, 0);
        PostMessageW(h, WM_APP_DONE, (WPARAM)STAGE_TRANSCRIBE, 0);
    }

    free(a);
    return 0;

cancelled:
    free(audioW);
    postError(h, L"Cancelled.");
    free(a);
    return 0;

cleanup:
    free(audioW);
    free(a);
    return 0;
}
