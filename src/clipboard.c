/* ------------------------------------------------------------------ *
 *  clipboard.c  --  CopyTextToClipboard.
 *
 *  Copies a UTF-16 string onto the Windows clipboard as CF_UNICODETEXT.
 *  Ownership rule (the whole point of this module): once
 *  SetClipboardData succeeds the system owns the HGLOBAL, so we must
 *  NOT GlobalFree it; we only free on the failure path.
 * ------------------------------------------------------------------ */

#include "common.h"
#include <string.h>

void CopyTextToClipboard(HWND owner, const WCHAR *text)
{
    if (!text)
        return;

    size_t len   = wcslen(text);
    size_t bytes = (len + 1) * sizeof(WCHAR);

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hMem)
        return;

    void *dst = GlobalLock(hMem);
    if (!dst) {
        GlobalFree(hMem);
        return;
    }
    memcpy(dst, text, bytes);
    GlobalUnlock(hMem);

    if (!OpenClipboard(owner)) {
        GlobalFree(hMem);
        return;
    }
    if (!EmptyClipboard()) {
        CloseClipboard();
        GlobalFree(hMem);
        return;
    }
    if (!SetClipboardData(CF_UNICODETEXT, hMem)) {
        /* We still own the block on failure. */
        CloseClipboard();
        GlobalFree(hMem);
        return;
    }

    /* Success: the clipboard now owns hMem -- do NOT free it. */
    CloseClipboard();
}
