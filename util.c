/* util.c -- growable buffers and small helpers.  Copyright (C) 2026 David Walther.  GPL-3.0-or-later. */
#define _POSIX_C_SOURCE 200809L
#include "winless.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void die_oom(void) { fputs("winless: out of memory\n", stderr); exit(70); }

void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) die_oom(); return p; }
void *xrealloc(void *p, size_t n) { p = realloc(p, n ? n : 1); if (!p) die_oom(); return p; }
char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }
char *xstrndup(const char *s, size_t n) { char *p = xmalloc(n + 1); memcpy(p, s, n); p[n] = 0; return p; }

void sb_init(sb_t *b) { b->s = xmalloc(64); b->s[0] = 0; b->len = 0; b->cap = 64; }
void sb_free(sb_t *b) { free(b->s); b->s = NULL; b->len = b->cap = 0; }
static void sb_need(sb_t *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->len + extra + 1) cap *= 2;
    b->s = xrealloc(b->s, cap); b->cap = cap;
}
void sb_put(sb_t *b, const char *s, size_t n) { sb_need(b, n); memcpy(b->s + b->len, s, n); b->len += n; b->s[b->len] = 0; }
void sb_puts(sb_t *b, const char *s) { sb_put(b, s, strlen(s)); }
void sb_putc(sb_t *b, char c) { sb_put(b, &c, 1); }
void sb_printf(sb_t *b, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt); va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap); va_end(ap);
    if (n < 0) { va_end(ap2); return; }
    sb_need(b, (size_t)n);
    vsnprintf(b->s + b->len, (size_t)n + 1, fmt, ap2); va_end(ap2);
    b->len += (size_t)n;
}
char *sb_detach(sb_t *b, size_t *len) { char *p = b->s; if (len) *len = b->len; b->s = NULL; b->len = b->cap = 0; return p; }

/* Copy n bytes, replacing every invalid UTF-8 sequence (and NUL) with U+FFFD. */
void sb_put_utf8_clean(sb_t *b, const char *s, size_t n) {
    static const char repl[] = "\xEF\xBF\xBD";
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;
    while (i < n) {
        unsigned char c = p[i];
        size_t need, k;
        unsigned cp, min;
        if (c == 0) { sb_put(b, repl, 3); i++; continue; }
        if (c < 0x80) { sb_putc(b, (char)c); i++; continue; }
        if ((c & 0xE0) == 0xC0) { need = 1; cp = c & 0x1Fu; min = 0x80; }
        else if ((c & 0xF0) == 0xE0) { need = 2; cp = c & 0x0Fu; min = 0x800; }
        else if ((c & 0xF8) == 0xF0) { need = 3; cp = c & 0x07u; min = 0x10000; }
        else { sb_put(b, repl, 3); i++; continue; }
        if (n - i < need + 1) { sb_put(b, repl, 3); i++; continue; }          /* truncated sequence */
        for (k = 1; k <= need; k++) {
            if ((p[i + k] & 0xC0) != 0x80) break;
            cp = (cp << 6) | (p[i + k] & 0x3Fu);
        }
        if (k <= need || cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { sb_put(b, repl, 3); i++; continue; }
        sb_put(b, s + i, need + 1);
        i += need + 1;
    }
}

double now_monotonic(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    sb_t b; sb_init(&b);
    char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) sb_put(&b, buf, n);
    fclose(f);
    return sb_detach(&b, len);
}

/* Where viewer.html lives: $WINLESS_HTML, the installed data directory, or next to the executable (source tree). */
const char *viewer_html_path(void) {
    static char buf[4200];
    const char *env = getenv("WINLESS_HTML");
    if (env && *env && access(env, R_OK) == 0) return env;
#ifdef PKGDATADIR
    snprintf(buf, sizeof buf, "%s/viewer.html", PKGDATADIR);
    if (access(buf, R_OK) == 0) return buf;
#endif
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = 0;
        char *slash = strrchr(exe, '/');
        if (slash) { *slash = 0; snprintf(buf, sizeof buf, "%s/viewer.html", exe); if (access(buf, R_OK) == 0) return buf; }
    }
    return "viewer.html";
}
