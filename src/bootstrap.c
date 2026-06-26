#include "common.h"
#include <process.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdio.h>

/* Bridge (defined in proc.c): published so a long first-run download can
 * observe the Cancel/close request and abort. */
extern AppState *g_proc_cancel_app;

/* First-run download manifest.  Order MUST match the ASSET_* enum.
 * version: a tag recorded in installed.cfg.  When the build's pinned tag
 *   differs from what's recorded, that asset (and only it) is re-downloaded.
 *   yt-dlp uses L"" => it is freshness-checked against GitHub instead.
 * minBytes: a present dest smaller than this is treated as broken (re-fetch).
 *   Floors are well below real sizes so a valid file is never flagged
 *   (note llama-cli.exe is a ~9 KB thin launcher backed by *-impl.dll). */
static const AssetSpec g_assets[ASSET_COUNT] = {
    /* ASSET_YTDLP */ {
        L"https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe",
        NULL,
        L"bin\\yt-dlp.exe",
        NULL,
        L"ytdlp", L"", 1u * 1024 * 1024 },
    /* ASSET_FFMPEG */ {
        L"https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip",
        L"ffmpeg.zip",
        L"bin\\ffmpeg.exe",
        L"bin",
        L"ffmpeg", L"btbn-gpl-win64", 5u * 1024 * 1024 },
    /* ASSET_WHISPER */ {
        L"https://github.com/ggml-org/whisper.cpp/releases/download/v1.9.1/whisper-bin-x64.zip",
        L"whisper.zip",
        L"bin\\whisper\\whisper-cli.exe",
        L"bin\\whisper",
        L"whisper", L"v1.9.1", 64u * 1024 },
    /* ASSET_WMODEL */ {
        L"https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin",
        NULL,
        L"models\\ggml-base.en.bin",
        NULL,
        L"wmodel", L"ggml-base.en", 10u * 1024 * 1024 },
    /* ASSET_LLAMA */ {
        L"https://github.com/ggml-org/llama.cpp/releases/download/b9811/llama-b9811-bin-win-cpu-x64.zip",
        L"llama.zip",
        L"bin\\llama\\llama-completion.exe",
        L"bin\\llama",
        L"llama", L"b9811", 4u * 1024 },
    /* ASSET_LMODEL */ {
        L"https://huggingface.co/Qwen/Qwen2.5-3B-Instruct-GGUF/resolve/main/qwen2.5-3b-instruct-q4_k_m.gguf",
        NULL,
        L"models\\qwen2.5-3b-instruct-q4_k_m.gguf",
        NULL,
        L"lmodel", L"qwen2.5-3b-instruct-q4_k_m", 50u * 1024 * 1024 },
};

static const WCHAR *const g_assetNames[ASSET_COUNT] = {
    L"yt-dlp",
    L"ffmpeg",
    L"whisper.cpp",
    L"whisper model (base.en)",
    L"llama.cpp",
    L"summarizer model (Qwen2.5-3B)"
};

static void PostStr(HWND h, UINT msg, const WCHAR *text)
{
    WCHAR *s = _wcsdup(text);
    if (!s)
        return;
    if (!PostMessageW(h, msg, 0, (LPARAM)s))
        free(s);
}

static void PostStatusFmt(HWND h, const WCHAR *fmt, const WCHAR *name)
{
    WCHAR buf[256];
    _snwprintf(buf, ARRAYSIZE(buf), fmt, name);
    buf[ARRAYSIZE(buf) - 1] = 0;
    PostStr(h, WM_APP_STATUS, buf);
}

static void PostErr(HWND h, int idx)
{
    WCHAR buf[512];
    _snwprintf(buf, ARRAYSIZE(buf),
               L"Setup failed while installing %s.\r\n"
               L"Check your internet connection and relaunch the application to resume.",
               g_assetNames[idx]);
    buf[ARRAYSIZE(buf) - 1] = 0;
    PostStr(h, WM_APP_ERROR, buf);
}

/* Run OS-bundled bsdtar to unzip `archive` into `extractDir`. */
static BOOL ExtractZip(const WCHAR *archive, const WCHAR *extractDir)
{
    WCHAR cmd[2048];
    ProcResult pr;

    /* Resolve the OS-bundled bsdtar by absolute path; never let
     * CreateProcessW search the CWD/own-dir for a "tar.exe". */
    WCHAR tarExe[MAX_PATH];
    UINT  sn = GetSystemDirectoryW(tarExe, ARRAYSIZE(tarExe));
    if (sn == 0 || sn >= ARRAYSIZE(tarExe) || !PathAppendW(tarExe, L"tar.exe"))
        return FALSE;

    cmd[0] = 0;
    ArgAppend(cmd, ARRAYSIZE(cmd), tarExe);
    ArgAppend(cmd, ARRAYSIZE(cmd), L"-xf");
    ArgAppend(cmd, ARRAYSIZE(cmd), archive);
    ArgAppend(cmd, ARRAYSIZE(cmd), L"-C");
    ArgAppend(cmd, ARRAYSIZE(cmd), extractDir);

    ZeroMemory(&pr, sizeof(pr));
    BOOL ran = RunProcess(cmd, NULL, NULL, NULL, &pr);
    BOOL ok = ran && pr.exitCode == 0;
    free(pr.out);
    return ok;
}

/* BtbN ffmpeg zip nests <root>\bin\ffmpeg.exe -- lift it up to bin\ffmpeg.exe. */
static void FlattenFfmpeg(const AppPaths *p)
{
    WCHAR pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    PathUnder(p, pattern, L"bin\\ffmpeg-*");
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (fd.cFileName[0] == L'.')
            continue;

        WCHAR folder[MAX_PATH], srcExe[MAX_PATH], dstExe[MAX_PATH];
        PathCombineW(folder, p->bin, fd.cFileName);
        PathCombineW(srcExe, folder, L"bin\\ffmpeg.exe");
        if (FileExists(srcExe)) {
            PathCombineW(dstExe, p->bin, L"ffmpeg.exe");
            MoveFileExW(srcExe, dstExe, MOVEFILE_REPLACE_EXISTING);
        }
        break;
    } while (FindNextFileW(h, &fd));

    FindClose(h);
}

/* whisper-bin-x64.zip nests its exe + ggml DLLs under a "Release\" folder.
 * Lift bin\whisper\Release\* up to bin\whisper\ so whisper-cli.exe sits next
 * to the DLLs it loads and matches the expected dest path. */
static void FlattenWhisper(const AppPaths *p)
{
    WCHAR rel[MAX_PATH], pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    PathCombineW(rel, p->whisper, L"Release");
    if (!PathIsDirectoryW(rel))
        return;   /* future builds may already be flat */

    PathCombineW(pattern, rel, L"*");
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        WCHAR src[MAX_PATH], dst[MAX_PATH];
        PathCombineW(src, rel, fd.cFileName);
        PathCombineW(dst, p->whisper, fd.cFileName);
        MoveFileExW(src, dst, MOVEFILE_REPLACE_EXISTING);
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    RemoveDirectoryW(rel);
}

/* ---------------------- integrity / version state ---------------------- */

/* A present dest is only trusted if it is at least minBytes (catches a
 * 0-byte/truncated file that would otherwise pass a bare existence test). */
static BOOL FileSizeAtLeast(const WCHAR *path, DWORD minBytes)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad))
        return FALSE;
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        return FALSE;
    ULONGLONG sz = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    return sz >= (ULONGLONG)minBytes;
}

/* installed.cfg: one "key=value" line per asset, recording the version tag
 * that is actually installed.  Tiny, ASCII, kept beside the .exe. */
#define MAN_MAX 16
typedef struct { WCHAR key[64]; WCHAR val[160]; } ManEntry;
typedef struct { ManEntry e[MAN_MAX]; int n; } Manifest;

static void ManifestPath(const AppPaths *p, WCHAR *out)
{
    PathCombineW(out, p->exeDir, L"installed.cfg");
}

static const WCHAR *ManifestGet(const Manifest *m, const WCHAR *key)
{
    for (int i = 0; i < m->n; i++)
        if (wcscmp(m->e[i].key, key) == 0)
            return m->e[i].val;
    return NULL;
}

static void ManifestSet(Manifest *m, const WCHAR *key, const WCHAR *val)
{
    for (int i = 0; i < m->n; i++) {
        if (wcscmp(m->e[i].key, key) == 0) {
            lstrcpynW(m->e[i].val, val, ARRAYSIZE(m->e[i].val));
            return;
        }
    }
    if (m->n < MAN_MAX) {
        lstrcpynW(m->e[m->n].key, key, ARRAYSIZE(m->e[m->n].key));
        lstrcpynW(m->e[m->n].val, val, ARRAYSIZE(m->e[m->n].val));
        m->n++;
    }
}

static void ManifestLoad(const AppPaths *p, Manifest *m)
{
    m->n = 0;
    WCHAR path[MAX_PATH];
    ManifestPath(p, path);
    char *utf8 = ReadFileUtf8(path);
    if (!utf8) return;
    WCHAR *text = Utf8ToWide(utf8);
    free(utf8);
    if (!text) return;

    WCHAR *line = text;
    while (*line) {
        WCHAR *nl = line;
        while (*nl && *nl != L'\n' && *nl != L'\r') nl++;
        WCHAR saved = *nl;
        *nl = L'\0';
        WCHAR *eq = wcschr(line, L'=');
        if (eq && m->n < MAN_MAX) {
            *eq = L'\0';
            lstrcpynW(m->e[m->n].key, line, ARRAYSIZE(m->e[m->n].key));
            lstrcpynW(m->e[m->n].val, eq + 1, ARRAYSIZE(m->e[m->n].val));
            m->n++;
        }
        if (saved == L'\0') break;
        line = nl + 1;
    }
    free(text);
}

static void ManifestSave(const AppPaths *p, const Manifest *m)
{
    WCHAR buf[MAN_MAX * 240];
    buf[0] = L'\0';
    for (int i = 0; i < m->n; i++) {
        lstrcatW(buf, m->e[i].key);
        lstrcatW(buf, L"=");
        lstrcatW(buf, m->e[i].val);
        lstrcatW(buf, L"\r\n");
    }
    char *utf8 = WideToUtf8(buf);
    if (utf8) {
        WCHAR path[MAX_PATH];
        ManifestPath(p, path);
        WriteFileUtf8(path, utf8);
        free(utf8);
    }
}

/* "YYYYMMDD" local date, used to throttle the yt-dlp freshness check. */
static void TodayStamp(WCHAR *out9)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snwprintf(out9, 9, L"%04u%02u%02u", st.wYear, st.wMonth, st.wDay);
    out9[8] = L'\0';
}

/* Best-effort: fetch the "tag_name" of a GitHub repo's latest release.
 * Returns FALSE on any network/parse failure (caller keeps what it has). */
static BOOL GithubLatestTag(AppState *app, const WCHAR *apiUrl,
                            WCHAR *outTag, size_t cap)
{
    WCHAR jsonPath[MAX_PATH];
    PathCombineW(jsonPath, app->paths.temp, L"gh_latest.json");

    if (!DownloadFile(app->hMain, ASSET_YTDLP, apiUrl, jsonPath))
        return FALSE;

    char *body = ReadFileUtf8(jsonPath);
    DeleteFileW(jsonPath);
    if (!body)
        return FALSE;

    BOOL ok = FALSE;
    const char *p = strstr(body, "\"tag_name\"");
    if (p && (p = strchr(p, ':')) != NULL) {
        p++;
        while (*p == ' ' || *p == '\"') p++;
        char tag[64];
        int n = 0;
        while (*p && *p != '\"' && n < (int)sizeof(tag) - 1)
            tag[n++] = *p++;
        tag[n] = '\0';
        if (n > 0) {
            WCHAR *wtag = Utf8ToWide(tag);
            if (wtag) {
                lstrcpynW(outTag, wtag, (int)cap);
                free(wtag);
                ok = TRUE;
            }
        }
    }
    free(body);
    return ok;
}

/* The pinned llama build tag rotates and old tags get pruned.  If the
 * pinned zip 404s, ask GitHub for the current release tag and rebuild
 * the asset URL from it. */
static BOOL FetchLatestLlamaUrl(AppState *app, WCHAR *outUrl, size_t cap)
{
    WCHAR tag[64];
    if (!GithubLatestTag(app,
            L"https://api.github.com/repos/ggml-org/llama.cpp/releases/latest",
            tag, ARRAYSIZE(tag)))
        return FALSE;

    _snwprintf(outUrl, cap,
        L"https://github.com/ggml-org/llama.cpp/releases/download/%s/llama-%s-bin-win-cpu-x64.zip",
        tag, tag);
    outUrl[cap - 1] = L'\0';
    return TRUE;
}

static BOOL DownloadArchive(AppState *app, int idx, const WCHAR *url,
                            const WCHAR *arcAbs)
{
    if (DownloadFile(app->hMain, idx, url, arcAbs))
        return TRUE;

    /* llama tag may have rotated; retry with the current release. */
    if (idx == ASSET_LLAMA) {
        WCHAR liveUrl[1024];
        if (FetchLatestLlamaUrl(app, liveUrl, ARRAYSIZE(liveUrl)))
            return DownloadFile(app->hMain, idx, liveUrl, arcAbs);
    }
    return FALSE;
}

BOOL AssetsPresent(const AppPaths *p)
{
    for (int i = 0; i < ASSET_COUNT; i++) {
        WCHAR abs[MAX_PATH];
        PathUnder(p, abs, g_assets[i].dest);
        if (!FileExists(abs))
            return FALSE;
    }
    return TRUE;
}

/* Ask the installed yt-dlp what version it is (e.g. "2025.06.20") so a
 * pre-manifest install is adopted with its real tag instead of a guess. */
static BOOL GetInstalledYtdlpVersion(AppState *app, WCHAR *out, size_t cap)
{
    WCHAR exe[MAX_PATH], cmd[MAX_PATH + 32];
    PathCombineW(exe, app->paths.bin, L"yt-dlp.exe");

    cmd[0] = L'\0';
    ArgAppend(cmd, ARRAYSIZE(cmd), exe);
    ArgAppend(cmd, ARRAYSIZE(cmd), L"--version");

    ProcResult pr;
    ZeroMemory(&pr, sizeof pr);
    BOOL ran = RunProcess(cmd, app->paths.bin, NULL, NULL, &pr);

    BOOL ok = FALSE;
    if (ran && pr.exitCode == 0 && pr.out) {
        char ver[64];
        int n = 0;
        for (const char *p = pr.out;
             *p && *p != '\n' && *p != '\r' && *p != ' ' && n < (int)sizeof(ver) - 1;
             ++p)
            ver[n++] = *p;
        ver[n] = '\0';
        if (n > 0) {
            WCHAR *w = Utf8ToWide(ver);
            if (w) { lstrcpynW(out, w, (int)cap); free(w); ok = TRUE; }
        }
    }
    free(pr.out);
    return ok;
}

/* Decide whether asset i must be (re)downloaded, consulting the manifest.
 * Side effects: may adopt a pre-manifest file or refresh the yt-dlp check
 * date into *man (setting *manDirty), and writes the version tag to record
 * on a successful download into outVersion. */
static BOOL NeedsDownload(AppState *app, int i, Manifest *man,
                          BOOL *manDirty, WCHAR *outVersion, size_t verCap)
{
    const AssetSpec *spec = &g_assets[i];
    WCHAR destAbs[MAX_PATH];
    PathUnder(&app->paths, destAbs, spec->dest);

    BOOL present = FileExists(destAbs) && FileSizeAtLeast(destAbs, spec->minBytes);
    const WCHAR *have = ManifestGet(man, spec->key);

    lstrcpynW(outVersion, spec->version, (int)verCap);   /* default to record */

    if (i == ASSET_YTDLP) {
        const WCHAR *kApi =
            L"https://api.github.com/repos/yt-dlp/yt-dlp/releases/latest";
        WCHAR today[9]; TodayStamp(today);

        if (!present) {                       /* fresh install */
            WCHAR latest[64];
            if (GithubLatestTag(app, kApi, latest, ARRAYSIZE(latest))) {
                lstrcpynW(outVersion, latest, (int)verCap);
                ManifestSet(man, L"ytdlp_checked", today);
                *manDirty = TRUE;
            } else {
                lstrcpynW(outVersion, L"unknown", (int)verCap);
            }
            return TRUE;
        }

        if (!have) {                          /* adopt a pre-manifest install */
            WCHAR installed[64];
            if (GetInstalledYtdlpVersion(app, installed, ARRAYSIZE(installed)))
                ManifestSet(man, spec->key, installed);
            else
                ManifestSet(man, spec->key, L"adopted");
            *manDirty = TRUE;
            have = ManifestGet(man, spec->key);
        }

        const WCHAR *checked = ManifestGet(man, L"ytdlp_checked");
        if (!checked || wcscmp(checked, today) != 0) {   /* once per day */
            PostStr(app->hMain, WM_APP_STATUS, L"Checking yt-dlp for updates…");
            WCHAR latest[64];
            if (GithubLatestTag(app, kApi, latest, ARRAYSIZE(latest))) {
                ManifestSet(man, L"ytdlp_checked", today);
                *manDirty = TRUE;
                if (wcscmp(have, latest) != 0) {  /* adopted or behind -> update */
                    lstrcpynW(outVersion, latest, (int)verCap);
                    return TRUE;
                }
            }
            /* offline / parse failure: keep the installed yt-dlp */
        }
        return FALSE;
    }

    /* Heavy assets: pinned-version + integrity, never an online check. */
    if (!present)
        return TRUE;                          /* missing or truncated */
    if (!have) {                              /* present but unrecorded -> adopt */
        ManifestSet(man, spec->key, spec->version);
        *manDirty = TRUE;
        return FALSE;
    }
    if (spec->version[0] && wcscmp(have, spec->version) != 0)
        return TRUE;                          /* build bumped the pinned tag */
    return FALSE;                             /* present + current */
}

unsigned __stdcall BootstrapThread(void *args)
{
    WorkerArgs *wa  = (WorkerArgs *)args;
    HWND        hwnd = wa->hwnd;
    AppState   *app  = wa->app;

    /* Publish ourselves so DownloadFile/RunProcess can be cancelled. */
    g_proc_cancel_app = app;

    Manifest man;
    ManifestLoad(&app->paths, &man);
    BOOL manDirty = FALSE;

    for (int i = 0; i < ASSET_COUNT; i++) {
        if (app->cancelFlag) {     /* user closed/cancelled mid-setup */
            if (manDirty) ManifestSave(&app->paths, &man);
            free(wa);
            return 1;
        }

        const AssetSpec *spec = &g_assets[i];
        WCHAR recordVer[160];

        if (!NeedsDownload(app, i, &man, &manDirty, recordVer, ARRAYSIZE(recordVer))) {
            PostMessageW(hwnd, WM_APP_DL_DONE, (WPARAM)i, 0);
            continue;
        }

        WCHAR destAbs[MAX_PATH];
        PathUnder(&app->paths, destAbs, spec->dest);

        PostStatusFmt(hwnd, L"Downloading %s…", g_assetNames[i]);

        BOOL good;
        if (spec->archive == NULL) {
            /* Direct file straight to its destination. */
            good = DownloadFile(hwnd, i, spec->url, destAbs);
        } else {
            WCHAR arcAbs[MAX_PATH];
            PathCombineW(arcAbs, app->paths.temp, spec->archive);

            good = DownloadArchive(app, i, spec->url, arcAbs);
            if (good) {
                PostStatusFmt(hwnd, L"Extracting %s…", g_assetNames[i]);

                WCHAR exDir[MAX_PATH];
                PathUnder(&app->paths, exDir, spec->extractDir);
                CreateDirectoryW(exDir, NULL);

                good = ExtractZip(arcAbs, exDir);
                if (good && i == ASSET_FFMPEG)
                    FlattenFfmpeg(&app->paths);
                if (good && i == ASSET_WHISPER)
                    FlattenWhisper(&app->paths);

                DeleteFileW(arcAbs);

                if (good)
                    good = FileExists(destAbs);
            }
        }

        if (!good) {
            if (!app->cancelFlag)   /* a deliberate cancel is not a failure */
                PostErr(hwnd, i);
            if (manDirty) ManifestSave(&app->paths, &man);
            free(wa);
            return 1;
        }

        ManifestSet(&man, spec->key, recordVer);   /* record what we installed */
        ManifestSave(&app->paths, &man);
        manDirty = FALSE;

        PostMessageW(hwnd, WM_APP_DL_DONE, (WPARAM)i, 0);
    }

    if (manDirty) ManifestSave(&app->paths, &man);
    PostMessageW(hwnd, WM_APP_BOOT_DONE, 0, 0);
    free(wa);
    return 0;
}

/* ===================== on-demand GPU backends ===================== *
 *  Downloaded only when the user picks a GPU engine, into their own
 *  folders so they never collide with the CPU builds.
 * ----------------------------------------------------------------- */

/* Lift <parentAbs>\Release\* up into <parentAbs> (whisper zips nest there). */
static void FlattenReleaseDir(const WCHAR *parentAbs)
{
    WCHAR rel[MAX_PATH], pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    PathCombineW(rel, parentAbs, L"Release");
    if (!PathIsDirectoryW(rel)) return;
    PathCombineW(pattern, rel, L"*");
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        WCHAR s[MAX_PATH], d[MAX_PATH];
        PathCombineW(s, rel, fd.cFileName);
        PathCombineW(d, parentAbs, fd.cFileName);
        MoveFileExW(s, d, MOVEFILE_REPLACE_EXISTING);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    RemoveDirectoryW(rel);
}

/* Download `url` -> temp\arcName, extract into destDirAbs, optional flatten. */
static BOOL FetchExtract(AppState *app, HWND h, int idx, const WCHAR *url,
                         const WCHAR *arcName, const WCHAR *destDirAbs, BOOL flatten)
{
    WCHAR arc[MAX_PATH];
    PathCombineW(arc, app->paths.temp, arcName);
    CreateDirectoryW(destDirAbs, NULL);
    if (!DownloadFile(h, idx, url, arc)) return FALSE;
    BOOL ok = ExtractZip(arc, destDirAbs);
    DeleteFileW(arc);
    if (ok && flatten) FlattenReleaseDir(destDirAbs);
    return ok;
}

BOOL BackendNeeded(const AppState *app)
{
    WCHAR p[MAX_PATH];
    if (app->engTranscribe == ENGINE_CUDA) {
        PathUnder(&app->paths, p, L"bin\\whisper-cuda\\whisper-cli.exe");
        if (!FileExists(p)) return TRUE;
    }
    if (app->engSummarize == ENGINE_VULKAN) {
        PathUnder(&app->paths, p, L"bin\\llama-vulkan\\llama-completion.exe");
        if (!FileExists(p)) return TRUE;
    }
    if (app->engSummarize == ENGINE_CUDA) {
        PathUnder(&app->paths, p, L"bin\\llama-cuda\\llama-completion.exe");
        if (!FileExists(p)) return TRUE;
    }
    return FALSE;
}

unsigned __stdcall EnsureBackendThread(void *args)
{
    WorkerArgs *wa  = (WorkerArgs *)args;
    HWND        h   = wa->hwnd;
    AppState   *app = wa->app;
    g_proc_cancel_app = app;
    BOOL ok = TRUE;

    if (ok && app->engTranscribe == ENGINE_CUDA) {
        WCHAR dest[MAX_PATH], dir[MAX_PATH];
        PathUnder(&app->paths, dest, L"bin\\whisper-cuda\\whisper-cli.exe");
        if (!FileExists(dest)) {
            PostStr(h, WM_APP_STATUS, L"Downloading whisper CUDA backend (~677 MB)…");
            PathUnder(&app->paths, dir, L"bin\\whisper-cuda");
            ok = FetchExtract(app, h, ASSET_WHISPER,
                L"https://github.com/ggml-org/whisper.cpp/releases/download/v1.9.1/whisper-cublas-12.4.0-bin-x64.zip",
                L"whisper-cuda.zip", dir, TRUE) && FileExists(dest);
        }
    }
    if (ok && !app->cancelFlag && app->engSummarize == ENGINE_VULKAN) {
        WCHAR dest[MAX_PATH], dir[MAX_PATH];
        PathUnder(&app->paths, dest, L"bin\\llama-vulkan\\llama-completion.exe");
        if (!FileExists(dest)) {
            PostStr(h, WM_APP_STATUS, L"Downloading llama Vulkan backend (~32 MB)…");
            PathUnder(&app->paths, dir, L"bin\\llama-vulkan");
            ok = FetchExtract(app, h, ASSET_LLAMA,
                L"https://github.com/ggml-org/llama.cpp/releases/download/b9811/llama-b9811-bin-win-vulkan-x64.zip",
                L"llama-vulkan.zip", dir, FALSE) && FileExists(dest);
        }
    }
    if (ok && !app->cancelFlag && app->engSummarize == ENGINE_CUDA) {
        WCHAR dest[MAX_PATH], dir[MAX_PATH];
        PathUnder(&app->paths, dest, L"bin\\llama-cuda\\llama-completion.exe");
        if (!FileExists(dest)) {
            PathUnder(&app->paths, dir, L"bin\\llama-cuda");
            PostStr(h, WM_APP_STATUS, L"Downloading llama CUDA backend (~262 MB)…");
            ok = FetchExtract(app, h, ASSET_LLAMA,
                L"https://github.com/ggml-org/llama.cpp/releases/download/b9811/llama-b9811-bin-win-cuda-12.4-x64.zip",
                L"llama-cuda.zip", dir, FALSE);
            if (ok && !app->cancelFlag) {
                PostStr(h, WM_APP_STATUS, L"Downloading CUDA runtime (~391 MB)…");
                ok = FetchExtract(app, h, ASSET_LLAMA,
                    L"https://github.com/ggml-org/llama.cpp/releases/download/b9811/cudart-llama-bin-win-cuda-12.4-x64.zip",
                    L"cudart.zip", dir, FALSE);
            }
            ok = ok && FileExists(dest);
        }
    }

    if (!ok && !app->cancelFlag) {
        WCHAR *e = _wcsdup(L"Could not download the GPU backend. Check your "
                           L"connection. That engine stays on CPU until it succeeds.");
        if (e && !PostMessageW(h, WM_APP_ERROR, 0, (LPARAM)e)) free(e);
    } else {
        PostMessageW(h, WM_APP_DONE, (WPARAM)STAGE_BACKEND, 0);
    }
    free(wa);
    return ok ? 0 : 1;
}
