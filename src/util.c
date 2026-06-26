#include "common.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static void EnsureDir(const WCHAR *dir)
{
    if (dir && *dir && !CreateDirectoryW(dir, NULL)) {
        /* ERROR_ALREADY_EXISTS is fine; anything else we silently tolerate
           because the caller will fail later with a clearer error. */
    }
}

void ResolvePaths(AppPaths *p)
{
    if (GetModuleFileNameW(NULL, p->exeDir, MAX_PATH) == 0)
        p->exeDir[0] = L'\0';
    PathRemoveFileSpecW(p->exeDir);

    PathCombineW(p->bin,     p->exeDir, L"bin");
    PathCombineW(p->whisper, p->bin,    L"whisper");
    PathCombineW(p->llama,   p->bin,    L"llama");
    PathCombineW(p->models,  p->exeDir, L"models");
    PathCombineW(p->temp,    p->exeDir, L"temp");
    PathCombineW(p->output,  p->exeDir, L"output");

    EnsureDir(p->bin);
    EnsureDir(p->whisper);
    EnsureDir(p->llama);
    EnsureDir(p->models);
    EnsureDir(p->temp);
    EnsureDir(p->output);
}

void PathUnder(const AppPaths *p, WCHAR *out, const WCHAR *rel)
{
    if (!PathCombineW(out, p->exeDir, rel))
        out[0] = L'\0';
}

/* Strict gate: only http(s) URLs whose host is youtu.be or *.youtube.com,
   and with no whitespace / shell-hostile characters embedded. */
BOOL IsPlausibleYouTubeUrl(const WCHAR *u)
{
    if (!u) return FALSE;
    while (*u == L' ' || *u == L'\t') u++;

    size_t len = wcslen(u);
    if (len < 11 || len >= 2048) return FALSE;

    for (const WCHAR *q = u; *q; ++q) {
        WCHAR c = *q;
        if (c < 0x20 || c == L' ' || c == L'"' || c == L'\'' ||
            c == L'<'  || c == L'>' || c == L'|' || c == L'^' || c == L'`')
            return FALSE;
    }

    const WCHAR *p;
    if (_wcsnicmp(u, L"https://", 8) == 0)      p = u + 8;
    else if (_wcsnicmp(u, L"http://", 7) == 0)  p = u + 7;
    else return FALSE;

    WCHAR host[256];
    size_t hi = 0;
    while (*p && *p != L'/' && *p != L'?' && *p != L'#' && *p != L':' && hi < 255)
        host[hi++] = *p++;
    host[hi] = L'\0';

    if (_wcsicmp(host, L"youtu.be") == 0) return TRUE;

    const WCHAR *suf = L"youtube.com";
    size_t sl = wcslen(suf);
    size_t hl = wcslen(host);
    if (hl >= sl && _wcsnicmp(host + (hl - sl), suf, sl) == 0 &&
        (hl == sl || host[hl - sl - 1] == L'.'))
        return TRUE;

    return FALSE;
}

char *WideToUtf8(const WCHAR *w)
{
    if (!w) return NULL;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *s = (char *)malloc((size_t)n);
    if (!s) return NULL;
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) <= 0) {
        free(s);
        return NULL;
    }
    return s;
}

WCHAR *Utf8ToWide(const char *s)
{
    if (!s) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    WCHAR *w = (WCHAR *)malloc((size_t)n * sizeof(WCHAR));
    if (!w) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
        free(w);
        return NULL;
    }
    return w;
}

char *ReadFileUtf8(const WCHAR *path)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;

    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 ||
        size.QuadPart > 0x7FFFFFF0) {
        CloseHandle(h);
        return NULL;
    }

    size_t total = (size_t)size.QuadPart;
    char *buf = (char *)malloc(total + 1);
    if (!buf) { CloseHandle(h); return NULL; }

    size_t got = 0;
    while (got < total) {
        DWORD chunk = (DWORD)((total - got > 0x10000000) ? 0x10000000 : (total - got));
        DWORD rd = 0;
        if (!ReadFile(h, buf + got, chunk, &rd, NULL) || rd == 0) break;
        got += rd;
    }
    CloseHandle(h);
    buf[got] = '\0';

    /* Strip a UTF-8 BOM if present. */
    if (got >= 3 && (unsigned char)buf[0] == 0xEF &&
        (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) {
        memmove(buf, buf + 3, got - 3 + 1);
    }
    return buf;
}

BOOL WriteFileUtf8(const WCHAR *path, const char *utf8)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    BOOL ok = TRUE;
    size_t total = utf8 ? strlen(utf8) : 0;
    size_t put = 0;
    while (put < total) {
        DWORD chunk = (DWORD)((total - put > 0x10000000) ? 0x10000000 : (total - put));
        DWORD wr = 0;
        if (!WriteFile(h, utf8 + put, chunk, &wr, NULL) || wr == 0) { ok = FALSE; break; }
        put += wr;
    }
    CloseHandle(h);
    return ok;
}

BOOL FileExists(const WCHAR *path)
{
    DWORD a = GetFileAttributesW(path);
    return (a != INVALID_FILE_ATTRIBUTES) &&
           !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int CpuThreads(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int n = (int)si.dwNumberOfProcessors;
    if (n < 1) n = 1;
    if (n > 16) n = 16;
    return n;
}
