/* ------------------------------------------------------------------ *
 *  summarize.c -- SummarizeThread + hierarchical map-reduce.
 *
 *  Fully offline via the bundled llama-completion.exe + Qwen2.5-3B-Instruct
 *  Q4_K_M GGUF (context auto-sized per call).  Prompts are hand-built ChatML
 *  written to temp\_prompt.txt (UTF-8, no BOM) and fed with -f so transcript
 *  quotes/newlines and large bodies can never break the command line.
 *
 *    MAP     each chunk -> detailed factual bullet points (keep specifics)
 *    REDUCE  recursively merge/de-duplicate (keeping all distinct points)
 *            while the concatenation still exceeds the chunk budget
 *    FINAL   one pass -> overview + comprehensive key-point bullets
 *
 *  Most transcripts (<= SINGLE_PASS_CHARS) skip MAP/REDUCE entirely and go
 *  straight to one FINAL pass = a single model load.  Only very long videos
 *  fall back to map-reduce.  The output budget (-n) scales with the input so
 *  long videos get a fuller summary instead of a fixed cap.
 * ------------------------------------------------------------------ */

#include "common.h"
#include <process.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>

extern AppState *g_proc_cancel_app;

#define CHUNK_CHARS       12000u  /* map/reduce chunk size for very long transcripts */
#define OVERLAP_CHARS      200u
#define SINGLE_PASS_CHARS 48000u  /* at or under this, summarize in ONE pass (one model load) */

static const char *SYS_SUM     = "You are a precise transcript summarizer. You preserve the important details and never omit significant points.";
static const char *MAP_INSTR   = "Extract the key facts and points from this section of a transcript as detailed bullet points. Preserve names, numbers, technical terms, and specifics. Do not omit important details, and add nothing that is not present.";
static const char *REDUCE_INSTR= "Combine these notes into a single list of bullet points. Remove only exact duplicates; keep every distinct point and all specific details.";
static const char *FINAL_INSTR = "Write a thorough summary of the transcript: a short overview paragraph, then a comprehensive, well-organized set of bullet points covering all the main topics and important details in the order they appear. Do not omit significant points.";

static char *sdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *r = (char *)malloc(n);
    if (r) memcpy(r, s, n);
    return r;
}

static int clampi(long v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : (int)v); }

static char *join_strs(char **a, size_t n, const char *sep)
{
    size_t sl = strlen(sep), tot = 1;
    for (size_t i = 0; i < n; ++i) tot += strlen(a[i]) + (i ? sl : 0);
    char *r = (char *)malloc(tot);
    if (!r) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; ++i) {
        if (i) { memcpy(r + o, sep, sl); o += sl; }
        size_t li = strlen(a[i]);
        memcpy(r + o, a[i], li);
        o += li;
    }
    r[o] = '\0';
    return r;
}

/* Strip trailing ChatML/end markers and surrounding whitespace in place. */
static void rstrip_markers(char *s)
{
    static const char *m1 = "<|im_end|>";
    static const char *m2 = "[end of text]";
    size_t l1 = strlen(m1), l2 = strlen(m2);
    size_t n = strlen(s);
    for (;;) {
        while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' ||
                         s[n-1] == ' '  || s[n-1] == '\t'))
            s[--n] = '\0';
        if (n >= l1 && memcmp(s + n - l1, m1, l1) == 0) { n -= l1; s[n] = '\0'; continue; }
        if (n >= l2 && memcmp(s + n - l2, m2, l2) == 0) { n -= l2; s[n] = '\0'; continue; }
        break;
    }
}

/* Parse llama --perf lines off stderr to capture real tokens/sec, which
 * excludes the (constant) model-load time and so reflects engine speed. */
typedef struct { double prefill, gen; } PerfCtx;

static void perf_cb(void *ud, const char *line)
{
    PerfCtx *p = (PerfCtx *)ud;
    const char *tps = strstr(line, "tokens per second");
    if (!tps) return;
    const char *e = tps;
    while (e > line && e[-1] == ' ') e--;
    const char *s = e;
    while (s > line && ((s[-1] >= '0' && s[-1] <= '9') || s[-1] == '.')) s--;
    double v = atof(s);
    if (v <= 0.0) return;
    if (strstr(line, "prompt eval time")) p->prefill = v;
    else if (strstr(line, "eval time"))   p->gen     = v;
}

/* One llama-completion call.  Returns malloc'd assistant text (possibly empty),
 * or NULL only when the process could not be run / prompt not written. */
static char *llama_run(AppState *app, const char *sys, const char *instr, const char *text, int nPredict)
{
    if (!text) text = "";

    size_t need = strlen(sys) + strlen(instr) + strlen(text) + 128;
    char *prompt = (char *)malloc(need);
    if (!prompt) return NULL;
    snprintf(prompt, need,
        "<|im_start|>system\n%s<|im_end|>\n"
        "<|im_start|>user\n%s\n\n%s<|im_end|>\n"
        "<|im_start|>assistant\n",
        sys, instr, text);

    WCHAR ppath[MAX_PATH];
    _snwprintf(ppath, MAX_PATH, L"%s\\_prompt.txt", app->paths.temp);
    ppath[MAX_PATH - 1] = L'\0';

    size_t promptLen = strlen(prompt);
    BOOL wok = WriteFileUtf8(ppath, prompt);
    free(prompt);
    if (!wok) return NULL;

    WCHAR exe[MAX_PATH], dir[MAX_PATH], model[MAX_PATH], tnum[16], nglbuf[8];
    int ngl = 0;
    /* llama-completion.exe (NOT llama-cli.exe): in current llama.cpp builds
     * llama-cli is an interactive chat REPL that dumps a banner + command
     * list + the echoed prompt to stdout and rejects -no-cnv.  llama-completion
     * does a clean non-interactive completion: only generated text on stdout.
     * Exe folder + GPU-offload layer count come from the Encoder setting
     * (CPU=0 layers; Vulkan/CUDA=99 => offload everything). */
    LlamaExeFor(app, exe, dir, &ngl);
    _snwprintf(model, MAX_PATH, L"%s\\qwen2.5-3b-instruct-q4_k_m.gguf", app->paths.models);
    _snwprintf(tnum, 16, L"%d", CpuThreads());
    _snwprintf(nglbuf, 8, L"%d", ngl);
    model[MAX_PATH - 1] = nglbuf[7] = L'\0';

    /* Auto-size the context to the prompt: short map/reduce chunks stay light
     * and fast, while a whole-transcript single pass gets enough room. Capped
     * at the model's 32K training context; ~3 chars/token is a safe (high)
     * estimate so we never under-size and truncate the transcript. */
    int nctx = 2048;
    size_t estTok = promptLen / 3 + (size_t)nPredict + 256 /*margin*/;
    while ((size_t)nctx < estTok && nctx < 32768) nctx <<= 1;
    WCHAR cbuf[16];
    _snwprintf(cbuf, 16, L"%d", nctx);
    cbuf[15] = L'\0';

    WCHAR nbuf[16];
    _snwprintf(nbuf, 16, L"%d", nPredict);
    nbuf[15] = L'\0';

    WCHAR cmd[4096];
    cmd[0] = L'\0';
    ArgAppend(cmd, 4096, exe);
    ArgAppend(cmd, 4096, L"-m");      ArgAppend(cmd, 4096, model);
    ArgAppend(cmd, 4096, L"-c");      ArgAppend(cmd, 4096, cbuf);
    ArgAppend(cmd, 4096, L"-n");      ArgAppend(cmd, 4096, nbuf);
    ArgAppend(cmd, 4096, L"-t");      ArgAppend(cmd, 4096, tnum);
    ArgAppend(cmd, 4096, L"-ngl");    ArgAppend(cmd, 4096, nglbuf);
    ArgAppend(cmd, 4096, L"--temp");  ArgAppend(cmd, 4096, L"0.3");
    ArgAppend(cmd, 4096, L"--top-p"); ArgAppend(cmd, 4096, L"0.9");
    ArgAppend(cmd, 4096, L"-no-cnv");             /* no chat REPL, just complete */
    ArgAppend(cmd, 4096, L"--no-display-prompt"); /* don't echo the prompt       */
    ArgAppend(cmd, 4096, L"--no-warmup");         /* skip the empty warm-up run   */
    ArgAppend(cmd, 4096, L"--perf");              /* emit tokens/sec on stderr    */
    ArgAppend(cmd, 4096, L"-f");      ArgAppend(cmd, 4096, ppath);

    PerfCtx perf = { 0.0, 0.0 };
    ProcResult pr; ZeroMemory(&pr, sizeof pr);
    if (!RunProcess(cmd, dir, perf_cb, &perf, &pr))
        return NULL;
    app->sumPrefillTps = perf.prefill;
    app->sumGenTps     = perf.gen;

    char *out = pr.out ? pr.out : sdup("");
    if (!out) return NULL;

    size_t lead = 0;
    while (out[lead] == '\n' || out[lead] == '\r' ||
           out[lead] == ' '  || out[lead] == '\t')
        ++lead;
    if (lead) memmove(out, out + lead, strlen(out + lead) + 1);
    rstrip_markers(out);
    return out;
}

char *SummarizeTranscript(AppState *app, HWND ui, const char *t)
{
    if (!t) return NULL;

    char *joined = NULL;
    size_t tlen = strlen(t);

    if (tlen <= SINGLE_PASS_CHARS) {
        /* Common case: the whole transcript fits one pass -> one model load. */
        joined = sdup(t);
        if (!joined) return NULL;
    } else {
        /* ---- MAP ---- */
        size_t n = 0;
        char **chunks = ChunkText(t, CHUNK_CHARS, OVERLAP_CHARS, &n);
        if (!chunks || n == 0) { free(chunks); return NULL; }

        char **P = (char **)malloc(n * sizeof(char *));
        size_t np = 0;
        if (!P) {
            for (size_t i = 0; i < n; ++i) free(chunks[i]);
            free(chunks);
            return NULL;
        }
        for (size_t i = 0; i < n; ++i) {
            if (app->cancelFlag) {
                for (size_t k = i; k < n; ++k) free(chunks[k]);
                free(chunks);
                for (size_t k = 0; k < np; ++k) free(P[k]);
                free(P);
                return NULL;
            }
            char *part = llama_run(app, SYS_SUM, MAP_INSTR, chunks[i],
                                   clampi((long)(strlen(chunks[i]) / 12), 384, 1024));
            free(chunks[i]);
            if (part && *part) P[np++] = part; else free(part);
            PostMessageW(ui, WM_APP_PROGRESS, (WPARAM)(((i + 1) * 85) / n), 0);
        }
        free(chunks);
        if (np == 0) { free(P); return NULL; }

        joined = join_strs(P, np, "\n\n");
        for (size_t i = 0; i < np; ++i) free(P[i]);
        free(P);
        if (!joined) return NULL;

        /* ---- REDUCE (recursive, bounded) ---- */
        int level = 0;
        while (strlen(joined) > CHUNK_CHARS && level < 6) {
            if (app->cancelFlag) { free(joined); return NULL; }
            PostMessageW(ui, WM_APP_PROGRESS, (WPARAM)PROGRESS_MARQUEE, 0);

            size_t nc = 0;
            char **pieces = ChunkText(joined, CHUNK_CHARS, OVERLAP_CHARS, &nc);
            free(joined);
            joined = NULL;
            if (!pieces || nc == 0) { free(pieces); joined = sdup(""); break; }

            char **R = (char **)malloc(nc * sizeof(char *));
            size_t nr = 0;
            if (!R) {
                for (size_t i = 0; i < nc; ++i) free(pieces[i]);
                free(pieces);
                return NULL;
            }
            for (size_t i = 0; i < nc; ++i) {
                if (!app->cancelFlag) {
                    char *r = llama_run(app, SYS_SUM, REDUCE_INSTR, pieces[i],
                                        clampi((long)(strlen(pieces[i]) / 12), 384, 1024));
                    if (r && *r) R[nr++] = r; else free(r);
                }
                free(pieces[i]);
            }
            free(pieces);

            if (app->cancelFlag) {
                for (size_t i = 0; i < nr; ++i) free(R[i]);
                free(R);
                return NULL;
            }
            joined = join_strs(R, nr, "\n\n");
            for (size_t i = 0; i < nr; ++i) free(R[i]);
            free(R);
            if (!joined) return NULL;

            ++level;
            if (nc <= 1) break;
        }
    }

    if (app->cancelFlag) { free(joined); return NULL; }

    /* ---- FINAL ---- */
    PostMessageW(ui, WM_APP_PROGRESS, (WPARAM)PROGRESS_MARQUEE, 0);
    char *final = llama_run(app, SYS_SUM, FINAL_INSTR, joined,
                            clampi((long)(tlen / 22), 768, 1600));
    free(joined);
    return final;
}

/* Summarize wall-time is roughly linear in transcript size, so we store a
 * single self-calibrating rate (ms per 1000 transcript chars) next to the
 * exe and use it to drive the ETA.  First run uses a sane default. */
static double LoadSummRate(AppState *app)
{
    WCHAR p[MAX_PATH];
    _snwprintf(p, MAX_PATH, L"%s\\summ_perf.cfg", app->paths.exeDir);
    p[MAX_PATH - 1] = L'\0';
    char *s = ReadFileUtf8(p);
    double r = s ? atof(s) : 0.0;
    free(s);
    if (r < 200.0 || r > 200000.0) r = 9000.0;   /* default ms / 1000 chars */
    return r;
}

static void SaveSummRate(AppState *app, double msPerKchar)
{
    if (msPerKchar < 200.0 || msPerKchar > 200000.0) return;
    double next = LoadSummRate(app) * 0.5 + msPerKchar * 0.5;   /* smooth */
    char buf[64];
    snprintf(buf, sizeof buf, "%.1f", next);
    WCHAR p[MAX_PATH];
    _snwprintf(p, MAX_PATH, L"%s\\summ_perf.cfg", app->paths.exeDir);
    p[MAX_PATH - 1] = L'\0';
    WriteFileUtf8(p, buf);
}

unsigned __stdcall SummarizeThread(void *argp)
{
    WorkerArgs *a = (WorkerArgs *)argp;
    AppState *app = a->app;
    HWND h = a->hwnd;

    g_proc_cancel_app = app;
    app->sumPrefillTps = app->sumGenTps = 0.0;

    char *utf8 = (a->transcript) ? WideToUtf8(a->transcript) : NULL;
    char *sum = NULL;
    if (utf8 && *utf8) {
        size_t tchars = strlen(utf8);
        ULONGLONG estMs = (ULONGLONG)((double)tchars / 1000.0 * LoadSummRate(app));
        if (estMs < 3000) estMs = 3000;
        PostMessageW(h, WM_APP_OPBEGIN, (WPARAM)2, (LPARAM)estMs);

        ULONGLONG t0 = GetTickCount64();
        sum = SummarizeTranscript(app, h, utf8);
        ULONGLONG dt = GetTickCount64() - t0;
        /* Only calibrate from single-pass runs so the rate stays consistent
         * (map-reduce reloads the model per chunk and would skew it slow). */
        if (sum && *sum && !app->cancelFlag &&
            tchars >= 500 && tchars <= SINGLE_PASS_CHARS)
            SaveSummRate(app, (double)dt / ((double)tchars / 1000.0));
    }
    free(utf8);

    if (app->cancelFlag) {
        WCHAR *d = _wcsdup(L"Cancelled.");
        if (d && !PostMessageW(h, WM_APP_ERROR, 0, (LPARAM)d)) free(d);
    } else if (!sum || !*sum) {
        WCHAR *d = _wcsdup(L"Summarization failed.");
        if (d && !PostMessageW(h, WM_APP_ERROR, 0, (LPARAM)d)) free(d);
    } else {
        WCHAR *w = Utf8ToWide(sum);
        if (w) { if (!PostMessageW(h, WM_APP_SUMMARY, 0, (LPARAM)w)) free(w); }
        PostMessageW(h, WM_APP_PROGRESS, 100, 0);
        PostMessageW(h, WM_APP_DONE, (WPARAM)STAGE_SUMMARIZE, 0);
    }

    free(sum);
    free(a->transcript);
    free(a);
    return 0;
}
