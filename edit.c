/* edit.c -- editing a text by (row, column) ranges.  Copyright (C) 2026 David Walther.  GPL-3.0-or-later.
 *
 * Rows are the lines of the text, counted from 1 (a row is every stretch between newlines, so a text ending in a
 * newline has a final, empty row).  Columns count Unicode code points from 1; column (length + 1) is the end of the row.
 * A range is [begin, end): the end position is not included.  The text is assumed to be valid UTF-8 (the CLI cleans it).
 */
#define _POSIX_C_SOURCE 200809L
#include "winless.h"
#include <string.h>

/* Code points in the first n bytes of s that are lead bytes. */
static size_t cp_len(unsigned char c) { return c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1; }

size_t edit_offset(const char *text, size_t len, long row, long col) {
    if (row < 1 || col < 1) return (size_t)-1;
    size_t i = 0;
    for (long r = 1; r < row; r++) {                      /* skip to the start of the row */
        const char *nl = memchr(text + i, '\n', len - i);
        if (!nl) return (row == r + 1 && col == 1 && i < len) ? len : (size_t)-1;   /* just past an unterminated last row */
        i = (size_t)(nl - text) + 1;
    }
    for (long c = 1; c < col; c++) {                      /* walk code points; the row ends at '\n' or the end of the text */
        if (i >= len || text[i] == '\n') return (size_t)-1;
        i += cp_len((unsigned char)text[i]);
        if (i > len) return (size_t)-1;
    }
    return i;
}

char *edit_replace(const char *text, size_t len, long r1, long c1, long r2, long c2, const char *repl, size_t rlen,
                   size_t *outlen, char **removed, size_t *removedlen) {
    size_t a = edit_offset(text, len, r1, c1), b = edit_offset(text, len, r2, c2);
    if (a == (size_t)-1 || b == (size_t)-1 || a > b) return NULL;
    sb_t out; sb_init(&out);
    sb_put(&out, text, a); sb_put(&out, repl, rlen); sb_put(&out, text + b, len - b);
    if (removed) { *removed = xstrndup(text + a, b - a); if (removedlen) *removedlen = b - a; }
    return sb_detach(&out, outlen);
}

void edit_endpos(long r1, long c1, const char *repl, size_t rlen, long *r2, long *c2) {
    long rows = 0; size_t cps = 0;
    for (size_t i = 0; i < rlen;) {
        if (repl[i] == '\n') { rows++; cps = 0; i++; continue; }
        i += cp_len((unsigned char)repl[i]); cps++;
    }
    if (rows == 0) { *r2 = r1; *c2 = c1 + (long)cps; }
    else { *r2 = r1 + rows; *c2 = 1 + (long)cps; }
}
