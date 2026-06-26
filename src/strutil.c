/* ------------------------------------------------------------------ *
 *  strutil.c -- pure UTF-8 text shaping.
 *
 *  CleanTranscript : single-pass cleanup of whisper's .txt output --
 *                    strip BOM, drop bracket/paren-only segment lines
 *                    ([BLANK_AUDIO]/[Music]/(music)/...), trim, collapse
 *                    runs of spaces, drop whisper's leading space, merge
 *                    consecutive segment lines into flowing paragraphs,
 *                    keep at most one blank line between paragraphs, and
 *                    normalize EOL to CRLF for the Win32 EDIT control.
 *  ChunkText       : split UTF-8 into <=maxChars pieces, preferring
 *                    sentence ('. '/'! '/'? ') or newline boundaries,
 *                    with a back-overlap carry so context isn't lost
 *                    across chunk seams.  Never splits a UTF-8 codepoint.
 * ------------------------------------------------------------------ */

#include "common.h"
#include <stdlib.h>
#include <string.h>

char *CleanTranscript(const char *in)
{
    if (!in) return NULL;

    const unsigned char *p = (const unsigned char *)in;
    if (p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) p += 3;

    size_t inLen = strlen((const char *)p);
    char *tmp = (char *)malloc(inLen + 1);          /* one cleaned line   */
    char *out = (char *)malloc(inLen * 2 + 16);     /* LF-separated body  */
    if (!tmp || !out) { free(tmp); free(out); return NULL; }

    size_t olen = 0;
    int midPara = 0;
    const unsigned char *s = p;

    while (*s) {
        const unsigned char *e = s;
        while (*e && *e != '\n') ++e;

        const unsigned char *ls = s, *le = e;
        if (le > ls && le[-1] == '\r') --le;
        while (ls < le && (*ls == ' ' || *ls == '\t')) ++ls;
        while (le > ls && (le[-1] == ' ' || le[-1] == '\t')) --le;

        /* Collapse internal whitespace runs into single spaces.  ASCII
         * space/tab never occur inside a UTF-8 multibyte sequence, so
         * byte-level handling is codepoint-safe. */
        size_t tl = 0;
        int prevSpace = 0;
        for (const unsigned char *q = ls; q < le; ++q) {
            unsigned char c = *q;
            if (c == ' ' || c == '\t') {
                if (!prevSpace) { tmp[tl++] = ' '; prevSpace = 1; }
            } else {
                tmp[tl++] = (char)c;
                prevSpace = 0;
            }
        }
        tmp[tl] = '\0';

        int drop = 0;
        if (tl == 0) {
            drop = 1;   /* blank -> paragraph break */
        } else if ((tmp[0] == '[' && tmp[tl - 1] == ']') ||
                   (tmp[0] == '(' && tmp[tl - 1] == ')')) {
            drop = 2;   /* bracket/paren-only -> discard, keep flow */
        }

        if (drop == 1) {
            if (midPara) { out[olen++] = '\n'; out[olen++] = '\n'; midPara = 0; }
        } else if (drop == 2) {
            /* skip entirely */
        } else {
            if (midPara) out[olen++] = ' ';
            memcpy(out + olen, tmp, tl);
            olen += tl;
            midPara = 1;
        }

        if (*e == 0) break;
        s = e + 1;
    }

    out[olen] = '\0';
    while (olen > 0 && (out[olen - 1] == '\n' || out[olen - 1] == ' ' ||
                        out[olen - 1] == '\t'))
        out[--olen] = '\0';

    /* Normalize LF -> CRLF. */
    char *fin = (char *)malloc(olen * 2 + 1);
    if (!fin) { free(tmp); free(out); return NULL; }
    size_t fo = 0;
    for (size_t i = 0; i < olen; ++i) {
        if (out[i] == '\n') { fin[fo++] = '\r'; fin[fo++] = '\n'; }
        else fin[fo++] = out[i];
    }
    fin[fo] = '\0';

    free(tmp);
    free(out);
    return fin;
}

char **ChunkText(const char *utf8, size_t maxChars, size_t overlap, size_t *count)
{
    if (count) *count = 0;
    if (!utf8) return NULL;

    if (maxChars < 16) maxChars = 16;
    if (overlap >= maxChars) overlap = maxChars / 4;

    size_t total = strlen(utf8);

    char **arr = NULL;
    size_t n = 0, cap = 0;

    if (total == 0) {
        arr = (char **)malloc(sizeof(char *));
        if (!arr) return NULL;
        arr[0] = (char *)malloc(1);
        if (!arr[0]) { free(arr); return NULL; }
        arr[0][0] = '\0';
        if (count) *count = 1;
        return arr;
    }

    size_t pos = 0;
    while (pos < total) {
        size_t end = pos + maxChars;
        if (end > total) end = total;

        if (end < total) {
            /* Prefer the latest sentence/newline boundary inside (pos,end]. */
            size_t b = 0;
            for (size_t i = end; i > pos + 1; --i) {
                char c = utf8[i - 1];
                if (c == '\n') { b = i; break; }
                if (c == ' ' &&
                    (utf8[i - 2] == '.' || utf8[i - 2] == '!' || utf8[i - 2] == '?')) {
                    b = i;
                    break;
                }
            }
            if (b > pos && b <= end) {
                end = b;
            } else {
                /* Hard cut: don't slice a UTF-8 codepoint. */
                while (end > pos && ((unsigned char)utf8[end] & 0xC0) == 0x80) --end;
            }
        }
        if (end <= pos) {
            end = (pos + maxChars <= total) ? pos + maxChars : total;
            if (end <= pos) end = total;
        }

        size_t len = end - pos;
        char *piece = (char *)malloc(len + 1);
        if (!piece) break;
        memcpy(piece, utf8 + pos, len);
        piece[len] = '\0';

        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            char **na = (char **)realloc(arr, cap * sizeof(char *));
            if (!na) { free(piece); break; }
            arr = na;
        }
        arr[n++] = piece;

        if (end >= total) break;

        size_t next = end;
        if (overlap < (end - pos)) next = end - overlap;
        if (next <= pos) next = end;
        while (next > pos && next < total && ((unsigned char)utf8[next] & 0xC0) == 0x80)
            ++next;
        pos = next;
    }

    if (count) *count = n;
    return arr;
}
