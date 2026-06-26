#include "common.h"

/* Same bridge proc.c uses: lets a long download observe the Cancel/close
 * request and abort, since a WinHTTP download has no child process for the
 * Cancel path to TerminateProcess. */
extern AppState *g_proc_cancel_app;

BOOL DownloadFile(HWND hwnd, int assetIndex, LPCWSTR url, LPCWSTR destPath)
{
    HINTERNET hSession = NULL, hConnect = NULL, hRequest = NULL;
    HANDLE    hFile = INVALID_HANDLE_VALUE;
    BOOL      ok = FALSE;

    WCHAR partPath[MAX_PATH];
    WCHAR host[256];
    WCHAR urlPath[2048];
    WCHAR extra[2048];
    WCHAR object[4200];
    URL_COMPONENTS uc;

    lstrcpynW(partPath, destPath, MAX_PATH);
    if ((size_t)lstrlenW(partPath) + 5 < MAX_PATH)
        lstrcatW(partPath, L".part");

    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize     = sizeof(uc);
    uc.lpszHostName     = host;     uc.dwHostNameLength   = ARRAYSIZE(host);
    uc.lpszUrlPath      = urlPath;  uc.dwUrlPathLength    = ARRAYSIZE(urlPath);
    uc.lpszExtraInfo    = extra;    uc.dwExtraInfoLength  = ARRAYSIZE(extra);
    if (!WinHttpCrackUrl(url, 0, 0, &uc))
        return FALSE;

    object[0] = 0;
    lstrcpynW(object, urlPath, ARRAYSIZE(object));
    if ((size_t)lstrlenW(object) + (size_t)lstrlenW(extra) < ARRAYSIZE(object))
        lstrcatW(object, extra);

    hSession = WinHttpOpen(L"YTTranscript/1.0",
                           WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession)
        goto done;

    {
        DWORD msTimeout = 60000;
        WinHttpSetTimeouts(hSession, 30000, 30000, msTimeout, msTimeout);
    }

    hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    if (!hConnect)
        goto done;

    {
        DWORD secFlag = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        hRequest = WinHttpOpenRequest(hConnect, L"GET", object, NULL,
                                      WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, secFlag);
    }
    if (!hRequest)
        goto done;

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        goto done;
    if (!WinHttpReceiveResponse(hRequest, NULL))
        goto done;

    {
        DWORD status = 0, size = sizeof(status);
        if (!WinHttpQueryHeaders(hRequest,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                WINHTTP_NO_HEADER_INDEX))
            goto done;
        if (status != 200)
            goto done;
    }

    ULONGLONG total = 0;
    {
        DWORD lenSize = sizeof(total);
        if (!WinHttpQueryHeaders(hRequest,
                WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER64,
                WINHTTP_HEADER_NAME_BY_INDEX, &total, &lenSize,
                WINHTTP_NO_HEADER_INDEX))
            total = 0;
    }

    hFile = CreateFileW(partPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        goto done;

    {
        ULONGLONG got = 0;
        int lastPct = -2;
        BYTE buf[65536];

        for (;;) {
            if (g_proc_cancel_app && g_proc_cancel_app->cancelFlag)
                goto done;   /* aborted: ok stays FALSE, .part is deleted */

            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &avail))
                goto done;
            if (avail == 0)
                break;

            DWORD toRead = (avail > sizeof(buf)) ? (DWORD)sizeof(buf) : avail;
            DWORD read = 0;
            if (!WinHttpReadData(hRequest, buf, toRead, &read))
                goto done;
            if (read == 0)
                break;

            DWORD written = 0;
            if (!WriteFile(hFile, buf, read, &written, NULL) || written != read)
                goto done;

            got += read;

            int pct;
            if (total)
                pct = (int)((got * 100ULL) / total);
            else
                pct = -1;
            if (pct != lastPct) {
                lastPct = pct;
                PostMessageW(hwnd, WM_APP_DL_PROGRESS,
                             (WPARAM)(INT_PTR)pct, (LPARAM)assetIndex);
            }
        }
        ok = TRUE;
    }

done:
    if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);

    if (ok) {
        DeleteFileW(destPath);
        if (!MoveFileExW(partPath, destPath, MOVEFILE_REPLACE_EXISTING))
            ok = FALSE;
    }
    if (!ok)
        DeleteFileW(partPath);

    return ok;
}
