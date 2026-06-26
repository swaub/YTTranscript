/* ------------------------------------------------------------------ *
 *  proc.c -- safe subprocess execution for YTTranscript.
 *
 *  ArgAppend  : CommandLineToArgvW-correct quoting/escaping so a hostile
 *               argument (the YouTube URL, paths with spaces) can never
 *               inject extra argv tokens.
 *  RunProcess : CreateProcessW with two redirected pipes.  A dedicated
 *               reader thread drains the child's stdout into a growable
 *               buffer while the caller's thread drains stderr line by
 *               line into a LineCb -- two independent readers so neither
 *               child pipe buffer can fill and deadlock.  The live child
 *               HANDLE is published into the AppState behind its
 *               CRITICAL_SECTION so the Cancel button can TerminateProcess
 *               it.  No shell is ever involved.
 * ------------------------------------------------------------------ */

#include "common.h"
#include <process.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Bridge so RunProcess (whose signature carries no AppState) can register
 * the live child for cancellation.  Set by the worker thread before it
 * starts running children; only one worker runs at a time. */
AppState *g_proc_cancel_app = NULL;

/* --------------------------- ArgAppend --------------------------- */

void ArgAppend(WCHAR *dst, size_t cap, const WCHAR *arg)
{
    if (!dst || cap == 0) return;
    size_t n = wcslen(dst);

#define PUT(ch) do { if (n + 1 < cap) dst[n++] = (WCHAR)(ch); } while (0)

    if (n > 0) PUT(L' ');

    BOOL needQuote = (arg == NULL || arg[0] == L'\0');
    if (arg) {
        for (const WCHAR *p = arg; *p; ++p) {
            WCHAR c = *p;
            if (c == L' ' || c == L'\t' || c == L'\n' || c == L'\v' || c == L'"') {
                needQuote = TRUE;
                break;
            }
        }
    }

    if (!needQuote) {
        for (const WCHAR *p = arg; p && *p; ++p) PUT(*p);
    } else {
        PUT(L'"');
        for (const WCHAR *p = arg; p && *p; ) {
            unsigned backslashes = 0;
            while (*p == L'\\') { ++backslashes; ++p; }
            if (*p == L'\0') {
                for (unsigned i = 0; i < backslashes * 2; ++i) PUT(L'\\');
                break;
            } else if (*p == L'"') {
                for (unsigned i = 0; i < backslashes * 2 + 1; ++i) PUT(L'\\');
                PUT(L'"');
                ++p;
            } else {
                for (unsigned i = 0; i < backslashes; ++i) PUT(L'\\');
                PUT(*p);
                ++p;
            }
        }
        PUT(L'"');
    }

    if (n < cap) dst[n] = L'\0';
    else dst[cap - 1] = L'\0';

#undef PUT
}

/* --------------------------- pipe readers ------------------------ */

typedef struct {
    HANDLE  h;       /* read end of child's stdout                    */
    char   *buf;     /* growable, NUL-terminated                      */
    size_t  len;
    size_t  cap;
} StdoutReader;

static unsigned __stdcall stdout_reader(void *p)
{
    StdoutReader *r = (StdoutReader *)p;
    char tmp[8192];
    DWORD got = 0;

    for (;;) {
        if (!ReadFile(r->h, tmp, (DWORD)sizeof tmp, &got, NULL) || got == 0)
            break;
        if (r->len + (size_t)got + 1 > r->cap) {
            size_t nc = r->cap ? r->cap * 2 : 16384;
            while (nc < r->len + (size_t)got + 1) nc *= 2;
            char *nb = (char *)realloc(r->buf, nc);
            if (!nb) break;
            r->buf = nb;
            r->cap = nc;
        }
        memcpy(r->buf + r->len, tmp, got);
        r->len += got;
        r->buf[r->len] = '\0';
    }
    return 0;
}

/* Drain stderr on the calling thread, emitting one line at a time.
 * A line ends at '\n' or '\r' (CLIs use '\r' for in-place progress);
 * blank runs are collapsed so callbacks see only non-empty lines. */
static void drain_stderr(HANDLE h, LineCb cb, void *ud)
{
    char *line = NULL;
    size_t llen = 0, lcap = 0;
    char tmp[8192];
    DWORD got = 0;

    for (;;) {
        if (!ReadFile(h, tmp, (DWORD)sizeof tmp, &got, NULL) || got == 0)
            break;
        for (DWORD i = 0; i < got; ++i) {
            char c = tmp[i];
            if (c == '\n' || c == '\r') {
                if (llen > 0) {
                    line[llen] = '\0';
                    if (cb) cb(ud, line);
                    llen = 0;
                }
                continue;
            }
            if (llen + 2 > lcap) {
                size_t nc = lcap ? lcap * 2 : 512;
                char *nb = (char *)realloc(line, nc);
                if (!nb) goto done;
                line = nb;
                lcap = nc;
            }
            line[llen++] = c;
        }
    }
    if (llen > 0 && cb) {
        line[llen] = '\0';
        cb(ud, line);
    }
done:
    free(line);
}

/* --------------------------- RunProcess -------------------------- */

BOOL RunProcess(WCHAR *cmdline, const WCHAR *workdir,
                LineCb on_stderr, void *ud, ProcResult *res)
{
    if (res) { res->exitCode = (DWORD)-1; res->out = NULL; res->outLen = 0; }
    if (!cmdline) return FALSE;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    HANDLE outRd = NULL, outWr = NULL, errRd = NULL, errWr = NULL, nul = INVALID_HANDLE_VALUE;

    if (!CreatePipe(&outRd, &outWr, &sa, 0))
        return FALSE;
    if (!CreatePipe(&errRd, &errWr, &sa, 0)) {
        CloseHandle(outRd); CloseHandle(outWr);
        return FALSE;
    }
    /* Read ends must NOT be inherited by the child. */
    SetHandleInformation(outRd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRd, HANDLE_FLAG_INHERIT, 0);

    /* Give the child a real (NUL) stdin so console tools don't choke. */
    nul = CreateFileW(L"NUL", GENERIC_READ,
                      FILE_SHARE_READ | FILE_SHARE_WRITE,
                      &sa, OPEN_EXISTING, 0, NULL);

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput  = nul;
    si.hStdOutput = outWr;
    si.hStdError  = errWr;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof pi);

    BOOL ok = CreateProcessW(NULL, cmdline, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL, workdir, &si, &pi);

    /* Parent no longer needs the write ends or its stdin handle; closing
     * the write ends is what lets the readers eventually see EOF. */
    CloseHandle(outWr);
    CloseHandle(errWr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);

    if (!ok) {
        CloseHandle(outRd);
        CloseHandle(errRd);
        return FALSE;
    }

    /* Publish the live child for the Cancel button. */
    if (g_proc_cancel_app) {
        EnterCriticalSection(&g_proc_cancel_app->cs);
        g_proc_cancel_app->hChild = pi.hProcess;
        LeaveCriticalSection(&g_proc_cancel_app->cs);
    }

    StdoutReader rd;
    rd.h = outRd; rd.buf = NULL; rd.len = 0; rd.cap = 0;
    HANDLE hRdr = (HANDLE)_beginthreadex(NULL, 0, stdout_reader, &rd, 0, NULL);

    if (!hRdr) {
        /* Without a concurrent stdout drainer, serially draining stderr
         * then stdout can deadlock (child blocks filling whichever pipe we
         * are not reading).  Treat the rare thread-creation failure as
         * fatal: kill the child and report failure. */
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
        if (g_proc_cancel_app) {
            EnterCriticalSection(&g_proc_cancel_app->cs);
            g_proc_cancel_app->hChild = NULL;
            LeaveCriticalSection(&g_proc_cancel_app->cs);
        }
        CloseHandle(outRd);
        CloseHandle(errRd);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        free(rd.buf);
        return FALSE;
    }

    /* Drain stderr here so progress flows live while stdout is buffered. */
    drain_stderr(errRd, on_stderr, ud);

    WaitForSingleObject(hRdr, INFINITE);
    CloseHandle(hRdr);

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &code);

    if (g_proc_cancel_app) {
        EnterCriticalSection(&g_proc_cancel_app->cs);
        g_proc_cancel_app->hChild = NULL;
        LeaveCriticalSection(&g_proc_cancel_app->cs);
    }

    CloseHandle(outRd);
    CloseHandle(errRd);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (res) {
        res->exitCode = code;
        res->out = rd.buf;        /* may be NULL when child wrote nothing */
        res->outLen = rd.len;
    } else {
        free(rd.buf);
    }
    return TRUE;
}
