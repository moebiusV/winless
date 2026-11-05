/* server.c -- the winless HTTP/SSE server: one thread, one poll() loop.
 * Copyright (C) 2026 David Walther.  GPL-3.0-or-later.
 *
 * Endpoints:  GET /  /ping  /content  /status  /wait  /events (Server-Sent Events)    POST /event  /cmd
 */
#define _POSIX_C_SOURCE 200809L
#include "winless.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_BODY (256UL * 1024 * 1024)
#define SSE_PING_SECS 4.0
#define MAX_SSE_BACKLOG (8UL * 1024 * 1024)

enum { ST_REQ, ST_SSE, ST_WAIT, ST_CLOSING };

typedef struct conn {
    int fd, state;
    sb_t in, out;
    size_t outpos;
    long wait_since;
    double wait_deadline, last_write;
} conn;

typedef struct { long a, b; char *color; } mark_t;
typedef struct { char *pattern; char *color; } hl_t;

static struct {
    char *name; char *text; size_t textlen; long version;
    char *theme, *font; long size; int numbers, wrap;           /* numbers/wrap: -1 = unset */
    mark_t *marks; size_t nmarks;
    hl_t *hls; size_t nhls;
    char *search; long top;
    char *view; size_t viewlen;
    long seq; char *last_event;
    char *path; long long mtime; long saved_depth;     /* the file being edited, its mtime when read, undo depth at the last write */
} H;

static int is_dirty(void);
static size_t nundo_get(void);
static long long file_mtime_ns(const char *path);
static void undo_clear(void);

static conn **conns;
static size_t nconns, capconns;
static volatile sig_atomic_t g_stop;

static void on_term(int sig) { (void)sig; g_stop = 1; }
static int g_listen_port;           /* for checking the Host and Origin of requests */
static double g_idle_secs;          /* > 0 for a server made for one window: it ends with that window */
static double g_quit_at;            /* when the window said quit (:q!, :wq, q), the time the server ends */

/* ------------------------------------------------------------------------------------------------------- hub */
static void hub_init(void) {
    memset(&H, 0, sizeof H);
    H.name = xstrdup(""); H.text = xstrdup(""); H.numbers = H.wrap = -1; H.top = 1; H.mtime = -1;
}

static void set_str(char **dst, const jv *v) {
    if (!v || v->t != J_STR) return;
    free(*dst); *dst = xstrndup(v->str, v->slen);
}

static void apply_settings(const jv *o) {
    if (!o || o->t != J_OBJ) return;
    set_str(&H.theme, json_get(o, "theme"));
    set_str(&H.font, json_get(o, "font"));
    const jv *v = json_get(o, "size");
    if (v && v->t == J_NUM) H.size = (long)v->num;
    v = json_get(o, "numbers"); if (v && v->t == J_BOOL) H.numbers = v->b;
    v = json_get(o, "wrap"); if (v && v->t == J_BOOL) H.wrap = v->b;
}

static void clear_marks(void) { for (size_t i = 0; i < H.nmarks; i++) free(H.marks[i].color); free(H.marks); H.marks = NULL; H.nmarks = 0; }
static void clear_hls(void) { for (size_t i = 0; i < H.nhls; i++) { free(H.hls[i].pattern); free(H.hls[i].color); } free(H.hls); H.hls = NULL; H.nhls = 0; }

static void put_opt_string(sb_t *b, const char *s) { if (s) json_put_string(b, s, strlen(s)); else sb_puts(b, "null"); }

static void settings_json(sb_t *b) {
    int first = 1;
    sb_putc(b, '{');
#define SEP() do { if (!first) sb_putc(b, ','); first = 0; } while (0)
    if (H.theme) { SEP(); sb_puts(b, "\"theme\":"); json_put_string(b, H.theme, strlen(H.theme)); }
    if (H.font) { SEP(); sb_puts(b, "\"font\":"); json_put_string(b, H.font, strlen(H.font)); }
    if (H.size) { SEP(); sb_printf(b, "\"size\":%ld", H.size); }
    if (H.numbers >= 0) { SEP(); sb_printf(b, "\"numbers\":%s", H.numbers ? "true" : "false"); }
    if (H.wrap >= 0) { SEP(); sb_printf(b, "\"wrap\":%s", H.wrap ? "true" : "false"); }
#undef SEP
    sb_putc(b, '}');
}

/* ---------------------------------------------------------------------------------------------- connections */
static size_t sse_count(void) {
    size_t n = 0;
    for (size_t i = 0; i < nconns; i++) if (conns[i]->state == ST_SSE) n++;
    return n;
}

static conn *conn_new(int fd) {
    conn *c = xmalloc(sizeof *c);
    memset(c, 0, sizeof *c);
    c->fd = fd; c->state = ST_REQ; sb_init(&c->in); sb_init(&c->out); c->last_write = now_monotonic();
    if (nconns == capconns) { capconns = capconns ? capconns * 2 : 16; conns = xrealloc(conns, capconns * sizeof *conns); }
    conns[nconns++] = c;
    return c;
}

static void conn_close(conn *c) {          /* the slot is reaped by compact() */
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}

static void compact(void) {
    size_t j = 0;
    for (size_t i = 0; i < nconns; i++) {
        if (conns[i]->fd < 0) { sb_free(&conns[i]->in); sb_free(&conns[i]->out); free(conns[i]); }
        else conns[j++] = conns[i];
    }
    nconns = j;
}

static void flush_conn(conn *c) {
    while (c->fd >= 0 && c->outpos < c->out.len) {
        ssize_t n = write(c->fd, c->out.s + c->outpos, c->out.len - c->outpos);
        if (n > 0) { c->outpos += (size_t)n; c->last_write = now_monotonic(); continue; }
        if (n < 0 && (errno == EINTR)) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        conn_close(c);
        return;
    }
    if (c->fd >= 0 && c->outpos >= c->out.len) {
        c->out.len = 0; c->out.s[0] = 0; c->outpos = 0;
        if (c->state == ST_CLOSING) conn_close(c);
    }
}

static void respond(conn *c, int code, const char *ctype, const char *body, size_t len) {
    const char *reason = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 403 ? "Forbidden" : code == 404 ? "Not Found" : "Internal Server Error";
    sb_printf(&c->out, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
              code, reason, ctype, len);
    sb_put(&c->out, body, len);
    c->state = ST_CLOSING;
}
static void respond_json(conn *c, sb_t *b) { respond(c, 200, "application/json; charset=utf-8", b->s, b->len); sb_free(b); }
static void respond_err(conn *c, int code, const char *msg) {
    sb_t b; sb_init(&b); sb_puts(&b, "{\"error\":"); json_put_string(&b, msg, strlen(msg)); sb_putc(&b, '}');
    respond(c, code, "application/json; charset=utf-8", b.s, b.len); sb_free(&b);
}

static void broadcast(const char *json, size_t n) {
    for (size_t i = 0; i < nconns; i++) {
        conn *c = conns[i];
        if (c->state != ST_SSE || c->fd < 0) continue;
        sb_puts(&c->out, "data: "); sb_put(&c->out, json, n); sb_puts(&c->out, "\n\n");
        flush_conn(c);
        if (c->fd >= 0 && c->out.len - c->outpos > MAX_SSE_BACKLOG) conn_close(c);   /* a client that never reads */
    }
}

/* --------------------------------------------------------------------------------------------------- requests */
static int qparam(const char *query, const char *key, double *out) {
    size_t kl = strlen(key);
    for (const char *p = query; p && *p;) {
        if (!strncmp(p, key, kl) && p[kl] == '=') { *out = atof(p + kl + 1); return 1; }
        p = strchr(p, '&'); if (p) p++;
    }
    return 0;
}

static void h_content(conn *c) {
    sb_t b; sb_init(&b);
    sb_puts(&b, "{\"name\":"); json_put_string(&b, H.name, strlen(H.name));
    sb_puts(&b, ",\"text\":"); json_put_string(&b, H.text, H.textlen);
    sb_printf(&b, ",\"version\":%ld,\"dirty\":%s,\"undo\":%zu,\"path\":", H.version, is_dirty() ? "true" : "false", nundo_get()); put_opt_string(&b, H.path);
    sb_puts(&b, ",\"settings\":"); settings_json(&b);
    sb_puts(&b, ",\"marks\":[");
    for (size_t i = 0; i < H.nmarks; i++) {
        if (i) sb_putc(&b, ',');
        sb_printf(&b, "{\"a\":%ld,\"b\":%ld,\"color\":", H.marks[i].a, H.marks[i].b); put_opt_string(&b, H.marks[i].color); sb_putc(&b, '}');
    }
    sb_puts(&b, "],\"hls\":[");
    for (size_t i = 0; i < H.nhls; i++) {
        if (i) sb_putc(&b, ',');
        sb_puts(&b, "{\"pattern\":"); json_put_string(&b, H.hls[i].pattern, strlen(H.hls[i].pattern));
        sb_puts(&b, ",\"color\":"); put_opt_string(&b, H.hls[i].color); sb_putc(&b, '}');
    }
    sb_puts(&b, "],\"annotations\":");
    { sb_t an; sb_init(&an); const char *e = H.path ? store_list_json(&an, H.path, 0) : "none";
      if (e) sb_puts(&b, "[]"); else sb_put(&b, an.s, an.len); sb_free(&an); }          /* a database problem must not break the page */
    sb_puts(&b, ",\"search\":"); put_opt_string(&b, H.search);
    sb_printf(&b, ",\"top\":%ld}", H.top);
    respond_json(c, &b);
}

static void h_status(conn *c) {
    sb_t b; sb_init(&b);
    sb_printf(&b, "{\"clients\":%zu,\"view\":", sse_count());
    if (H.view) sb_put(&b, H.view, H.viewlen); else sb_puts(&b, "{}");
    sb_printf(&b, ",\"version\":%ld,\"seq\":%ld,\"last_event\":", H.version, H.seq);
    put_opt_string(&b, H.last_event);
    sb_puts(&b, ",\"name\":"); json_put_string(&b, H.name, strlen(H.name)); sb_putc(&b, '}');
    respond_json(c, &b);
}

static void wait_reply(conn *c) {
    sb_t b; sb_init(&b);
    sb_printf(&b, "{\"seq\":%ld,\"event\":", H.seq); put_opt_string(&b, H.last_event);
    sb_printf(&b, ",\"timeout\":%s}", H.seq <= c->wait_since ? "true" : "false");
    respond_json(c, &b);
}

static void h_events(conn *c) {
    sb_puts(&c->out, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n: connected\n\n");
    c->state = ST_SSE;
}

static void h_index(conn *c) {
    size_t len;
    char *html = read_file(viewer_html_path(), &len);
    if (!html) { respond_err(c, 500, "cannot read viewer.html (set WINLESS_HTML)"); return; }
    respond(c, 200, "text/html; charset=utf-8", html, len);
    free(html);
}

static void h_event(conn *c, const char *body, size_t blen) {
    jv *v = json_parse(body, blen);
    if (!v) { respond_err(c, 400, "bad json"); return; }
    const char *t = json_gets(v, "type", NULL);
    if (t && !strcmp(t, "state")) {
        free(H.view); H.view = xstrndup(body, blen); H.viewlen = blen;
        H.top = (long)json_getnum(v, "top", 1);
    } else if (t && (!strcmp(t, "quit") || !strcmp(t, "closed"))) {
        H.seq++; free(H.last_event); H.last_event = xstrdup(t);
        if (!strcmp(t, "quit") && g_idle_secs > 0) g_quit_at = now_monotonic() + 2.0;       /* quitting ends the window's server; unsaved edits are discarded, as when quitting vim */
    }
    json_free(v);
    respond(c, 200, "application/json; charset=utf-8", "{\"ok\":true}", 11);
}

/* ------------------------------------------------------------------------------------------------ editing */
typedef struct { long r1, c1, r2, c2; char *old; size_t oldlen; } undo_t;       /* the range now holding the new text, and what it replaced */
static undo_t *undo_stack; static size_t nundo, capundo;
static undo_t *redo_stack; static size_t nredo, capredo;      /* edits that were undone, for redo; any new edit empties it */

static long long file_mtime_ns(const char *path) {
    struct stat st;
    if (stat(path, &st)) return -1;
    return (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
}
static void redo_clear(void) {
    for (size_t i = 0; i < nredo; i++) free(redo_stack[i].old);
    nredo = 0;
}
static void undo_clear(void) {
    for (size_t i = 0; i < nundo; i++) free(undo_stack[i].old);
    nundo = 0; H.saved_depth = 0; redo_clear();
}
static void stack_push(undo_t **st, size_t *n, size_t *cap, undo_t u) {
    if (*n == *cap) { *cap = *cap ? *cap * 2 : 64; *st = xrealloc(*st, *cap * sizeof **st); }
    (*st)[(*n)++] = u;
}
static int is_dirty(void) { return (long)nundo != H.saved_depth; }
static size_t nundo_get(void) { return nundo; }

static void put_result(sb_t *r, const char *err) {
    sb_printf(r, "{\"ok\":%s", err ? "false" : "true");
    if (err) { sb_puts(r, ",\"error\":"); json_put_string(r, err, strlen(err)); }
    sb_printf(r, ",\"version\":%ld,\"dirty\":%s,\"undo\":%zu,\"redo\":%zu}", H.version, is_dirty() ? "true" : "false", nundo, nredo);
}
static void text_changed(void) {                    /* tell every page to re-read the text, keeping its place */
    H.version++;
    const char *m = "{\"cmd\":\"reload\"}";
    broadcast(m, strlen(m));
}

/* Write the text to the file by way of a temporary file in the same directory, so a crash cannot leave half a file. */
static const char *write_atomic(const char *path, const char *data, size_t len) {
    size_t pl = strlen(path);
    char *tmp = xmalloc(pl + 16); snprintf(tmp, pl + 16, "%s.winless.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { free(tmp); return "cannot create the temporary file next to the original"; }
    int ok = fwrite(data, 1, len, f) == len && fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = (fclose(f) == 0) && ok;
    struct stat st;
    if (ok && stat(path, &st) == 0) chmod(tmp, st.st_mode & 07777);
    if (ok && rename(tmp, path) != 0) ok = 0;
    if (!ok) unlink(tmp);
    free(tmp);
    return ok ? NULL : "could not write the file";
}


/* ------------------------------------------------------------------------------------------------ history
 * Every write first keeps what it is about to replace, so any write can be undone later with `revert` -- and a revert is a write,
 * so it keeps what it replaces too.  Snapshots live in ~/.local/share/winless/history/<hash>-<name>/NNNNNN.<time>.prev */
static void history_dir(const char *path, char *buf, size_t n) {
    unsigned long long h = 1469598103934665603ULL;                       /* FNV-1a of the full path */
    for (const char *p = path; *p; p++) { h ^= (unsigned char)*p; h *= 1099511628211ULL; }
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    char dir[1024]; winless_data_dir(dir, sizeof dir);
    char hd[1100]; snprintf(hd, sizeof hd, "%s/history", dir); mkdir(hd, 0755);
    char nm[80]; size_t k = 0; for (; base[k] && k < 60; k++) nm[k] = (base[k] == '/' || base[k] == '%') ? '_' : base[k]; nm[k] = 0;
    snprintf(buf, n, "%s/%016llx-%s", hd, h, nm); mkdir(buf, 0755);
    char pf[1300]; snprintf(pf, sizeof pf, "%s/PATH", buf);
    FILE *f = fopen(pf, "w"); if (f) { fprintf(f, "%s\n", path); fclose(f); }       /* which file this directory is about */
}
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static size_t history_list(const char *dir, char ***names) {         /* the snapshot file names, oldest first */
    DIR *d = opendir(dir); size_t n = 0, cap = 0; *names = NULL;
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 6 || strcmp(e->d_name + l - 5, ".prev") != 0) continue;
        if (n == cap) { cap = cap ? cap * 2 : 16; *names = xrealloc(*names, cap * sizeof **names); }
        (*names)[n++] = xstrdup(e->d_name);
    }
    closedir(d);
    qsort(*names, n, sizeof **names, cmp_str);
    return n;
}
static void history_free(char **names, size_t n) { for (size_t i = 0; i < n; i++) free(names[i]); free(names); }

/* Keep the file's present content, unless it is what is about to be written.  NULL on success (or nothing to keep). */
static const char *snapshot_current(const char *path, const char *newdata, size_t newlen) {
    size_t len; char *old = read_file(path, &len);
    if (!old) return NULL;                                           /* no file yet: nothing to lose */
    if (len == newlen && memcmp(old, newdata, len) == 0) { free(old); return NULL; }
    char dir[1500]; history_dir(path, dir, sizeof dir);
    char **names; size_t n = history_list(dir, &names); long seq = 1;
    if (n) seq = atol(names[n - 1]) + 1;
    history_free(names, n);
    char ts[32]; time_t now = time(NULL); struct tm tm; gmtime_r(&now, &tm); strftime(ts, sizeof ts, "%Y%m%dT%H%M%SZ", &tm);
    char f[1700], tmp[1720]; snprintf(f, sizeof f, "%s/%06ld.%s.prev", dir, seq, ts); snprintf(tmp, sizeof tmp, "%s.tmp", f);
    FILE *fp = fopen(tmp, "wb");
    int ok = fp && fwrite(old, 1, len, fp) == len && fflush(fp) == 0 && fsync(fileno(fp)) == 0;
    if (fp) ok = (fclose(fp) == 0) && ok;
    if (ok) ok = rename(tmp, f) == 0;
    if (!ok) unlink(tmp);
    free(old);
    return ok ? NULL : "cannot keep a copy of the old file (so it was not written)";
}
static void history_json(sb_t *r) {
    if (!H.path) { put_result(r, "no file: the text was not opened from a file"); return; }
    char dir[1500]; history_dir(H.path, dir, sizeof dir);
    char **names; size_t n = history_list(dir, &names);
    sb_puts(r, "{\"ok\":true,\"path\":"); json_put_string(r, H.path, strlen(H.path)); sb_puts(r, ",\"items\":[");
    for (size_t i = 0; i < n; i++) {
        char f[1700]; snprintf(f, sizeof f, "%s/%s", dir, names[i]); struct stat st; long long sz = stat(f, &st) == 0 ? (long long)st.st_size : -1;
        const char *dot = strchr(names[i], '.'); char ts[32] = ""; if (dot) { snprintf(ts, sizeof ts, "%.16s", dot + 1); }
        if (i) sb_putc(r, ',');
        sb_printf(r, "{\"n\":%ld,\"time\":", atol(names[i])); json_put_string(r, ts, strlen(ts)); sb_printf(r, ",\"bytes\":%lld}", sz);
    }
    sb_puts(r, "]}");
    history_free(names, n);
}
static const char *history_read(const char *path, long which, char **data, size_t *len) {   /* which = 0: the latest */
    char dir[1500]; history_dir(path, dir, sizeof dir);
    char **names; size_t n = history_list(dir, &names);
    const char *err = NULL; const char *pick = NULL;
    if (!n) err = "no earlier version of this file has been kept yet";
    else if (which == 0) pick = names[n - 1];
    else { for (size_t i = 0; i < n; i++) if (atol(names[i]) == which) pick = names[i]; if (!pick) err = "no such version (see the history)"; }
    if (pick) { char f[1700]; snprintf(f, sizeof f, "%s/%s", dir, pick); *data = read_file(f, len); if (!*data) err = "cannot read that version"; }
    history_free(names, n);
    return err;
}

static const char *do_write(int force) {
    if (!H.path) return "no file: the text was not opened from a file";
    if (!force && H.mtime != -1 && file_mtime_ns(H.path) != H.mtime) return "the file changed on disk since it was opened (use :w! to overwrite)";
    const char *err = snapshot_current(H.path, H.text, H.textlen);        /* keep what is there now, so the write can be reverted */
    if (err) return err;
    err = write_atomic(H.path, H.text, H.textlen);
    if (!err) { H.mtime = file_mtime_ns(H.path); H.saved_depth = (long)nundo; const char *m = "{\"cmd\":\"reload\"}"; broadcast(m, strlen(m)); }   /* pages drop their [+] */
    return err;
}

static const char *apply_edit(long r1, long c1, long r2, long c2, const char *tx, size_t tl) {
    size_t nl, rl; char *removed;
    char *nt = tx ? edit_replace(H.text, H.textlen, r1, c1, r2, c2, tx, tl, &nl, &removed, &rl) : NULL;
    if (!nt) return tx ? "range out of bounds" : "no text given";
    long er, ec; edit_endpos(r1, c1, tx, tl, &er, &ec);
    stack_push(&undo_stack, &nundo, &capundo, (undo_t){ r1, c1, er, ec, removed, rl });
    redo_clear();
    free(H.text); H.text = nt; H.textlen = nl;
    text_changed();
    return NULL;
}

/* Run `sh -c cmd` with `in` on its standard input, collecting its output; in the file's directory.  The server waits for it (one window,
 * one server), but never longer than `timeout` seconds.  Returns NULL, or a message; *status is the exit status. */
static const char *run_filter(const char *cmd, const char *cwd, const char *in, size_t inlen, double timeout, sb_t *out, sb_t *errtxt, int *status) {
    int pin[2], pout[2], perr[2];
    if (pipe(pin) < 0) return "cannot start the command";
    if (pipe(pout) < 0) { close(pin[0]); close(pin[1]); return "cannot start the command"; }
    if (pipe(perr) < 0) { close(pin[0]); close(pin[1]); close(pout[0]); close(pout[1]); return "cannot start the command"; }
    pid_t pid = fork();
    if (pid < 0) { close(pin[0]); close(pin[1]); close(pout[0]); close(pout[1]); close(perr[0]); close(perr[1]); return "cannot start the command"; }
    if (pid == 0) {
        dup2(pin[0], 0); dup2(pout[1], 1); dup2(perr[1], 2);
        for (int fd = 3; fd < 1024; fd++) close(fd);                  /* not the listening socket, not the clients */
        if (cwd && *cwd && chdir(cwd) != 0) { /* run where we are */ }
        if (H.path) setenv("WINLESS_FILE", H.path, 1);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(pin[0]); close(pout[1]); close(perr[1]);
    int fi = pin[1], fo = pout[0], fe = perr[0];
    fcntl(fi, F_SETFL, fcntl(fi, F_GETFL) | O_NONBLOCK); fcntl(fo, F_SETFL, fcntl(fo, F_GETFL) | O_NONBLOCK); fcntl(fe, F_SETFL, fcntl(fe, F_GETFL) | O_NONBLOCK);
    size_t done = 0; const size_t maxout = 64UL * 1024 * 1024;
    double deadline = now_monotonic() + timeout; const char *err = NULL;
    if (inlen == 0) { close(fi); fi = -1; }
    while (fo >= 0 || fe >= 0) {
        struct pollfd pf[3]; int n = 0, io = -1, ie = -1, ii = -1;
        if (fi >= 0) { pf[n].fd = fi; pf[n].events = POLLOUT; ii = n++; }
        if (fo >= 0) { pf[n].fd = fo; pf[n].events = POLLIN; io = n++; }
        if (fe >= 0) { pf[n].fd = fe; pf[n].events = POLLIN; ie = n++; }
        double left = deadline - now_monotonic();
        if (left <= 0) { err = "the command did not finish in time and was stopped"; break; }
        int rc = poll(pf, (nfds_t)n, (int)(left * 1000) + 1);
        if (rc < 0 && errno != EINTR) { err = "cannot wait for the command"; break; }
        if (ii >= 0 && (pf[ii].revents & (POLLOUT | POLLERR | POLLHUP))) {
            ssize_t w = write(fi, in + done, inlen - done);
            if (w > 0) done += (size_t)w;
            if ((w < 0 && errno != EAGAIN && errno != EINTR) || done >= inlen) { close(fi); fi = -1; }     /* a command that ignores its input is fine */
        }
        char buf[65536];
        if (io >= 0 && (pf[io].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t k = read(fo, buf, sizeof buf);
            if (k > 0) { if (out->len + (size_t)k > maxout) { err = "the command produced too much output"; break; } sb_put(out, buf, (size_t)k); }
            else if (k == 0 || (errno != EAGAIN && errno != EINTR)) { close(fo); fo = -1; }
        }
        if (ie >= 0 && (pf[ie].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t k = read(fe, buf, sizeof buf);
            if (k > 0) { if (errtxt->len < 2000) sb_put(errtxt, buf, (size_t)k); }
            else if (k == 0 || (errno != EAGAIN && errno != EINTR)) { close(fe); fe = -1; }
        }
    }
    if (fi >= 0) close(fi);
    if (fo >= 0) close(fo);
    if (fe >= 0) close(fe);
    if (err) kill(pid, SIGKILL);
    int st = 0; while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    *status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    return err;
}

/* filter: replace a range with the output of a shell command run on that range -- one undoable edit, and only if the command succeeds. */
static void filter_command(const jv *v, sb_t *r) {
    const char *cmd = json_gets(v, "command", NULL);
    if (!cmd || !*cmd) { put_result(r, "no command given"); return; }
    long r1 = (long)json_getnum(v, "r1", 0), c1 = (long)json_getnum(v, "c1", 0), r2 = (long)json_getnum(v, "r2", 0), c2 = (long)json_getnum(v, "c2", 0);
    size_t a = edit_offset(H.text, H.textlen, r1, c1), b = edit_offset(H.text, H.textlen, r2, c2);
    if (a == (size_t)-1 || b == (size_t)-1 || a > b) { put_result(r, "range out of bounds"); return; }
    char *cwd = NULL;
    if (H.path) { char *sl = strrchr(H.path, '/'); if (sl && sl != H.path) cwd = xstrndup(H.path, (size_t)(sl - H.path)); }
    sb_t out, er; sb_init(&out); sb_init(&er); int status = 0;
    double timeout = json_getnum(v, "timeout", 30); if (timeout < 0.1) timeout = 30;
    const char *err = run_filter(cmd, cwd, H.text + a, b - a, timeout, &out, &er, &status);
    free(cwd);
    if (!err && status != 0) {
        while (er.len && (er.s[er.len - 1] == '\n' || er.s[er.len - 1] == '\r')) er.s[--er.len] = 0;
        sb_t msg; sb_init(&msg); sb_printf(&msg, "the command failed (exit %d)%s%.300s", status, er.len ? ": " : "", er.s);
        put_result(r, msg.s); sb_free(&msg); sb_free(&out); sb_free(&er); return;           /* the text is untouched */
    }
    if (err) { put_result(r, err); sb_free(&out); sb_free(&er); return; }
    if (json_getbool(v, "linewise", 0) && out.len && out.s[out.len - 1] != '\n') sb_putc(&out, '\n');     /* whole lines stay whole lines */
    if (out.len == b - a && memcmp(out.s, H.text + a, out.len) == 0) put_result(r, NULL);                  /* nothing changed */
    else put_result(r, apply_edit(r1, c1, r2, c2, out.s, out.len));
    sb_free(&out); sb_free(&er);
}

/* annotate: a note on a range, kept in the database.  Inside one paragraph the selected text is kept; over several paragraphs only
 * which ones; for the whole file, neither. */
static void annotate_command(const jv *v, sb_t *r) {
    if (!H.path) { put_result(r, "no file: the text was not opened from a file"); return; }
    const char *note = json_gets(v, "note", NULL);
    const char *kind = json_gets(v, "kind", NULL); if (!kind || !*kind) kind = "note";
    if (strcmp(kind, "note") && strcmp(kind, "slop") && strcmp(kind, "keep")) { put_result(r, "the kind must be note, slop or keep"); return; }
    if ((!note || !*note) && !strcmp(kind, "note")) { put_result(r, "the note is empty"); return; }          /* slop and keep need no words */
    long id = 0; const char *err, *scope; char paras[48] = ""; char *sel = NULL;
    long r1 = (long)json_getnum(v, "r1", 0), c1 = (long)json_getnum(v, "c1", 0), r2 = (long)json_getnum(v, "r2", 0), c2 = (long)json_getnum(v, "c2", 0);
    if (json_getbool(v, "file", 0)) { scope = "file"; r1 = c1 = r2 = c2 = 0; }
    else {
        size_t a = edit_offset(H.text, H.textlen, r1, c1), b = edit_offset(H.text, H.textlen, r2, c2);
        if (a == (size_t)-1 || b == (size_t)-1 || a >= b) { put_result(r, "nothing is selected (or the range is out of bounds)"); return; }
        size_t end = H.textlen; if (end && H.text[end - 1] == '\n') end--;
        long last = (c2 == 1 && r2 > r1) ? r2 - 1 : r2;                    /* an end at the start of a row does not reach into that row */
        if (a == 0 && b >= end) { scope = "file"; r1 = c1 = r2 = c2 = 0; }
        else if (last == r1) { scope = "text"; size_t bl = b - a; if (bl && H.text[a + bl - 1] == '\n') bl--; sel = xstrndup(H.text + a, bl); }
        else { scope = "paragraphs"; snprintf(paras, sizeof paras, "%ld-%ld", r1, last); }
    }
    err = store_add(H.path, r1, c1, r2, c2, scope, sel, paras[0] ? paras : NULL, note ? note : "", kind, &id);
    free(sel);
    if (err) { put_result(r, err); return; }
    const char *m = "{\"cmd\":\"reload\"}"; broadcast(m, strlen(m));        /* pages redraw their annotations */
    sb_printf(r, "{\"ok\":true,\"id\":%ld,\"scope\":\"%s\",\"kind\":\"%s\"}", id, scope, kind);
}

/* ------------------------------------------------------------------------------------------------ revision
 * The author marks text (notes, slop, keep); `revise` hands the open notes to a model, which answers with a list of edits; every edit is
 * checked (edits.c) and shown as an island to accept or reject; `finish` applies the accepted ones as ONE undoable edit.  The model runs
 * as a child process that the server polls between requests, so the window never freezes while it thinks. */
typedef struct {
    int active; pid_t pid; int fin, fout, ferr;
    sb_t in, out, err; size_t inpos;
    double started, deadline, last_progress; int out_eof, err_eof;
} job_t;
static job_t g_job;

enum { RV_NONE, RV_RUNNING, RV_READY, RV_ERROR };
static struct {
    int state; char *error, *raw;
    char *base; size_t baselen; int base_dirty;                   /* the text the model was asked about, and whether it had unsaved edits */
    edit_item *edits; size_t nedits;
    island *isl; size_t nisl; int *decision; char **custom;       /* decision: 0 pending, 1 accepted, 2 rejected; custom: a replacement the author typed */
    keep_span *keep; size_t nkeep;
    long *ann; size_t nann;                                       /* the notes this revision answers */
    char scope[24], backend[16]; double started;
} RV;
static char g_backend[16];                                        /* set with the `backend` command; else $WINLESS_BACKEND; else claude */

static void rv_clear(void) {
    free(RV.error); free(RV.raw); free(RV.base);
    edits_free(RV.edits, RV.nedits); islands_free(RV.isl, RV.nisl);
    if (RV.custom) for (size_t i = 0; i < RV.nisl; i++) free(RV.custom[i]);
    free(RV.custom); free(RV.decision); free(RV.keep); free(RV.ann);
    memset(&RV, 0, sizeof RV);
}
static void tell_pages_review(void) { const char *m = "{\"cmd\":\"review\"}"; broadcast(m, strlen(m)); }

static void job_close_fds(void) {
    if (g_job.fin >= 0) close(g_job.fin);
    if (g_job.fout >= 0) close(g_job.fout);
    if (g_job.ferr >= 0) close(g_job.ferr);
    g_job.fin = g_job.fout = g_job.ferr = -1;
}
static void job_kill(void) {
    if (!g_job.active) return;
    kill(-g_job.pid, SIGKILL); kill(g_job.pid, SIGKILL);
    int st; waitpid(g_job.pid, &st, 0);
    job_close_fds(); sb_free(&g_job.in); sb_free(&g_job.out); sb_free(&g_job.err);
    memset(&g_job, 0, sizeof g_job);
}

static const char *job_start(char *const argv[], const char *input, size_t inlen, double timeout) {
    int pin[2], pout[2], perr[2];
    if (pipe(pin) < 0) return "cannot start the model command";
    if (pipe(pout) < 0) { close(pin[0]); close(pin[1]); return "cannot start the model command"; }
    if (pipe(perr) < 0) { close(pin[0]); close(pin[1]); close(pout[0]); close(pout[1]); return "cannot start the model command"; }
    char dir[1024]; winless_data_dir(dir, sizeof dir);
    pid_t pid = fork();
    if (pid < 0) { close(pin[0]); close(pin[1]); close(pout[0]); close(pout[1]); close(perr[0]); close(perr[1]); return "cannot start the model command"; }
    if (pid == 0) {
        setpgid(0, 0);                                            /* its own group, so a cancel stops whatever it started */
        dup2(pin[0], 0); dup2(pout[1], 1); dup2(perr[1], 2);
        for (int fd = 3; fd < 1024; fd++) close(fd);
        if (chdir(dir) != 0) { /* run where we are */ }          /* a neutral directory: not the book's, so no project instructions leak in */
        execvp(argv[0], argv);
        dprintf(2, "cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    close(pin[0]); close(pout[1]); close(perr[1]);
    memset(&g_job, 0, sizeof g_job);
    g_job.active = 1; g_job.pid = pid; g_job.fin = pin[1]; g_job.fout = pout[0]; g_job.ferr = perr[0];
    fcntl(g_job.fin, F_SETFL, fcntl(g_job.fin, F_GETFL) | O_NONBLOCK); fcntl(g_job.fout, F_SETFL, fcntl(g_job.fout, F_GETFL) | O_NONBLOCK); fcntl(g_job.ferr, F_SETFL, fcntl(g_job.ferr, F_GETFL) | O_NONBLOCK);
    sb_init(&g_job.in); sb_init(&g_job.out); sb_init(&g_job.err); sb_put(&g_job.in, input, inlen);
    g_job.started = now_monotonic(); g_job.deadline = g_job.started + timeout; g_job.last_progress = g_job.started;
    return NULL;
}

static void rv_fail(const char *msg) { free(RV.error); RV.error = xstrdup(msg); RV.state = RV_ERROR; tell_pages_review(); }

/* The model has finished (or failed): turn its reply into a review. */
static void rv_job_done(int status, int timed_out, const sb_t *out, const sb_t *err) {
    if (RV.state != RV_RUNNING) return;
    char tail[420]; size_t el = err->len, from = el > 300 ? el - 300 : 0;
    snprintf(tail, sizeof tail, "%s", err->s + from);
    for (size_t i = strlen(tail); i > 0 && (tail[i - 1] == '\n' || tail[i - 1] == '\r' || tail[i - 1] == ' '); i--) tail[i - 1] = 0;
    char msg[600];
    if (timed_out) { rv_fail("the model did not answer in time (raise $WINLESS_LLM_TIMEOUT, in seconds, if the file is large)"); return; }
    if (status == 127) { snprintf(msg, sizeof msg, "cannot run the model command: %s", tail); rv_fail(msg); return; }
    if (status != 0) { snprintf(msg, sizeof msg, "the model command failed (exit %d)%s%s", status, tail[0] ? ": " : "", tail); rv_fail(msg); return; }
    edit_item *e; size_t n;
    const char *perr = edits_parse(out->s, out->len, &e, &n);
    if (perr) {
        RV.raw = xstrndup(out->s, out->len < 4000 ? out->len : 4000);
        snprintf(msg, sizeof msg, "the model's reply could not be used: %s", perr); rv_fail(msg); return;
    }
    edits_verify(RV.base, RV.baselen, e, n, RV.keep, RV.nkeep);
    RV.edits = e; RV.nedits = n;
    RV.nisl = edits_islands(RV.base, RV.baselen, e, n, &RV.isl);
    RV.decision = xmalloc((RV.nisl ? RV.nisl : 1) * sizeof *RV.decision); memset(RV.decision, 0, (RV.nisl ? RV.nisl : 1) * sizeof *RV.decision);
    RV.custom = xmalloc((RV.nisl ? RV.nisl : 1) * sizeof *RV.custom); memset(RV.custom, 0, (RV.nisl ? RV.nisl : 1) * sizeof *RV.custom);
    RV.state = RV_READY; tell_pages_review();
}

/* Called between requests: feed the model its input, collect its output, notice when it is done or late. */
static void job_pump(void) {
    if (!g_job.active) return;
    double now = now_monotonic();
    if (g_job.fin >= 0) {
        while (g_job.inpos < g_job.in.len) {
            ssize_t w = write(g_job.fin, g_job.in.s + g_job.inpos, g_job.in.len - g_job.inpos);
            if (w > 0) { g_job.inpos += (size_t)w; continue; }
            if (w < 0 && (errno == EAGAIN || errno == EINTR)) break;
            g_job.inpos = g_job.in.len; break;                    /* the command stopped reading: that is its business */
        }
        if (g_job.inpos >= g_job.in.len) { close(g_job.fin); g_job.fin = -1; }
    }
    char buf[65536];
    for (int which = 0; which < 2; which++) {
        int fd = which ? g_job.ferr : g_job.fout; int *eof = which ? &g_job.err_eof : &g_job.out_eof; sb_t *dst = which ? &g_job.err : &g_job.out;
        while (!*eof) {
            ssize_t k = read(fd, buf, sizeof buf);
            if (k > 0) { if (!which || dst->len < 20000) sb_put(dst, buf, (size_t)k); continue; }
            if (k < 0 && errno == EINTR) continue;
            if (k < 0 && errno == EAGAIN) break;
            *eof = 1;
        }
    }
    int timed_out = 0, status = 0;
    if (g_job.out_eof && g_job.err_eof) { int st = 0; waitpid(g_job.pid, &st, 0); status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0); }
    else if (now >= g_job.deadline) { timed_out = 1; kill(-g_job.pid, SIGKILL); kill(g_job.pid, SIGKILL); int st; waitpid(g_job.pid, &st, 0); }
    else {
        if (now - g_job.last_progress >= 1.0) {
            g_job.last_progress = now;
            char m[120]; snprintf(m, sizeof m, "{\"cmd\":\"revise\",\"elapsed\":%.0f,\"bytes\":%zu}", now - g_job.started, g_job.out.len); broadcast(m, strlen(m));
        }
        return;
    }
    job_close_fds();
    sb_t out = g_job.out, err = g_job.err; sb_free(&g_job.in);
    memset(&g_job, 0, sizeof g_job);
    rv_job_done(status, timed_out, &out, &err);
    sb_free(&out); sb_free(&err);
}

/* ---- collecting the author's open notes for the request ---- */
static void add_keep(long id, size_t off, size_t len) {
    RV.keep = xrealloc(RV.keep, (RV.nkeep + 1) * sizeof *RV.keep);
    RV.keep[RV.nkeep].id = id; RV.keep[RV.nkeep].off = off; RV.keep[RV.nkeep].len = len; RV.nkeep++;
}
static int parse_rows(const char *s, long *a, long *b) { return s && sscanf(s, "%ld-%ld", a, b) == 2; }

static const char *rv_collect(const jv *v, sb_t *annjson) {
    long only = (long)json_getnum(v, "id", 0);
    int ranged = json_get(v, "r1") != NULL;
    long R1 = (long)json_getnum(v, "r1", 0), C1 = (long)json_getnum(v, "c1", 0), R2 = (long)json_getnum(v, "r2", 0), C2 = (long)json_getnum(v, "c2", 0);
    long Rlast = (C2 == 1 && R2 > R1) ? R2 - 1 : R2;
    sb_t list; sb_init(&list);
    const char *err = store_list_json(&list, H.path, 0);
    if (err) { sb_free(&list); return err; }
    jv *arr = json_parse(list.s, list.len); sb_free(&list);
    if (!arr || arr->t != J_ARR) { json_free(arr); return "cannot read the notes"; }
    sb_puts(annjson, "[");
    for (size_t i = 0; i < arr->n; i++) {
        const jv *it = arr->items[i];
        const char *status = json_gets(it, "status", NULL), *kind = json_gets(it, "kind", NULL), *scope = json_gets(it, "scope", NULL);
        if (!kind || !scope || (status && !strcmp(status, "done"))) continue;
        long id = (long)json_getnum(it, "id", 0), r1 = (long)json_getnum(it, "r1", 0), c1 = (long)json_getnum(it, "c1", 0), r2 = (long)json_getnum(it, "r2", 0), c2 = (long)json_getnum(it, "c2", 0);
        const char *text = json_gets(it, "text", NULL), *paras = json_gets(it, "paragraphs", NULL), *note = json_gets(it, "note", NULL);
        long pa = 0, pb = 0; int have_rows = !strcmp(scope, "paragraphs") && parse_rows(paras, &pa, &pb);
        if (!strcmp(kind, "keep")) {                              /* protected text: always collected, whatever the scope of this revision */
            if (!strcmp(scope, "text") && text && *text) {
                size_t a = edit_offset(H.text, H.textlen, r1, c1), b = edit_offset(H.text, H.textlen, r2, c2), tl = strlen(text);
                if (a != (size_t)-1 && b != (size_t)-1 && b > a && b - a >= tl && memcmp(H.text + a, text, tl) == 0) add_keep(id, a, tl);
                else for (size_t p = 0; p + tl <= H.textlen; p++) if (memcmp(H.text + p, text, tl) == 0) add_keep(id, p, tl);   /* it moved: protect every place it occurs */
            } else if (have_rows) {
                size_t a = edit_offset(H.text, H.textlen, pa, 1), b = edit_offset(H.text, H.textlen, pb + 1, 1);
                if (a != (size_t)-1) add_keep(id, a, (b == (size_t)-1 ? H.textlen : b) - a);
            } else continue;                                      /* a keep on the whole file protects nothing in particular */
        }
        int is_keep = !strcmp(kind, "keep");                       /* keeps are always sent, so the model knows what to leave alone */
        int in_scope = 1;
        if (is_keep) in_scope = 1;
        else if (only) in_scope = (id == only);
        else if (ranged) {
            if (!strcmp(scope, "text")) { in_scope = (r1 > R1 || (r1 == R1 && c1 >= C1)) && (r2 < R2 || (r2 == R2 && c2 <= C2)); }
            else if (have_rows) in_scope = pa >= R1 && pb <= Rlast;
            else in_scope = 0;
        }
        if (!in_scope) continue;
        if (annjson->len > 1) sb_putc(annjson, ',');
        sb_printf(annjson, "{\"id\":%ld,\"kind\":", id); json_put_string(annjson, kind, strlen(kind));
        if (!strcmp(scope, "text") && text) { sb_printf(annjson, ",\"where\":\"line %ld\",\"text\":", r1); json_put_string(annjson, text, strlen(text)); }
        else if (have_rows) {
            size_t a = edit_offset(H.text, H.textlen, pa, 1), b = edit_offset(H.text, H.textlen, pb + 1, 1);
            if (a == (size_t)-1) a = 0;
            if (b == (size_t)-1) b = H.textlen;
            size_t tl = b > a ? b - a : 0, cap = tl > 8000 ? 8000 : tl; while (cap && (H.text[a + cap - 1] & 0xC0) == 0x80 && cap < tl) cap--;
            sb_printf(annjson, ",\"where\":\"lines %ld-%ld\",\"text\":", pa, pb); json_put_string(annjson, H.text + a, cap);
        } else sb_puts(annjson, ",\"where\":\"the whole document\",\"text\":null");
        sb_puts(annjson, ",\"note\":"); json_put_string(annjson, note ? note : "", note ? strlen(note) : 0); sb_putc(annjson, '}');
        if (!is_keep) { RV.ann = xrealloc(RV.ann, (RV.nann + 1) * sizeof *RV.ann); RV.ann[RV.nann++] = id; }
    }
    sb_putc(annjson, ']');
    json_free(arr);
    return NULL;
}

static void revise_command(const jv *v, sb_t *r) {
    if (!H.path) { put_result(r, "no file: the text was not opened from a file, so it has no notes"); return; }
    if (g_job.active || RV.state == RV_RUNNING) { put_result(r, "a revision is already running"); return; }
    if (RV.state == RV_READY) { put_result(r, "there is a review waiting: finish it (Enter) or cancel it (Esc) first"); return; }
    rv_clear();
    RV.base = xstrndup(H.text, H.textlen); RV.baselen = H.textlen; RV.base_dirty = is_dirty();
    const char *which = json_gets(v, "backend", NULL); if (!which || !*which) which = g_backend[0] ? g_backend : getenv("WINLESS_BACKEND");
    if (!which || !*which) which = "claude";
    snprintf(RV.backend, sizeof RV.backend, "%s", which);
    snprintf(RV.scope, sizeof RV.scope, "%s", json_get(v, "id") ? "note" : json_get(v, "r1") ? "selection" : "file");
    sb_t annjson; sb_init(&annjson);
    const char *err = rv_collect(v, &annjson);
    if (err) { sb_free(&annjson); rv_clear(); put_result(r, err); return; }
    if (RV.nann == 0) { sb_free(&annjson); rv_clear(); put_result(r, "no open notes to revise (mark text with n or s first)"); return; }
    sb_t user; sb_init(&user); llm_user_message(&user, H.text, H.textlen, annjson.s); sb_free(&annjson);
    const char *cmd = getenv("WINLESS_LLM"); sb_t reqjson; sb_init(&reqjson);
    char **argv = NULL; char *shargv[4]; char *const *runargv = NULL; const char *input = NULL; size_t inlen = 0;
    if (cmd && *cmd) {                                            /* any command: the request as JSON on stdin, the reply on stdout */
        snprintf(RV.backend, sizeof RV.backend, "custom");
        llm_request_json(&reqjson, llm_system_prompt(), user.s, user.len, getenv("WINLESS_MODEL"));
        shargv[0] = "/bin/sh"; shargv[1] = "-c"; shargv[2] = (char *)cmd; shargv[3] = NULL; runargv = shargv; input = reqjson.s; inlen = reqjson.len;
    } else if (!strcmp(RV.backend, "claude")) {
        argv = llm_claude_argv(llm_system_prompt(), getenv("WINLESS_MODEL"), getenv("WINLESS_EFFORT")); runargv = argv; input = user.s; inlen = user.len;
    } else if (!strcmp(RV.backend, "api")) {
        sb_free(&user); sb_free(&reqjson); rv_clear(); put_result(r, "the api backend is not built yet: use the claude backend (:set backend claude)"); return;
    } else { sb_free(&user); sb_free(&reqjson); rv_clear(); put_result(r, "the backend must be claude or api"); return; }
    const char *tmo = getenv("WINLESS_LLM_TIMEOUT"); double timeout = tmo && atof(tmo) > 0 ? atof(tmo) : 900;
    err = job_start(runargv, input, inlen, timeout);
    llm_free_argv(argv); sb_free(&user); sb_free(&reqjson);
    if (err) { rv_clear(); put_result(r, err); return; }
    RV.state = RV_RUNNING; RV.started = now_monotonic();
    tell_pages_review();
    sb_printf(r, "{\"ok\":true,\"started\":true,\"notes\":%zu,\"backend\":\"%s\"}", RV.nann, RV.backend);
}

/* ---- the review ---- */
static const char *decision_name(int d) { return d == 1 ? "accepted" : d == 2 ? "rejected" : "pending"; }
static void rv_counts(long *a, long *rj, long *p) {
    *a = *rj = *p = 0;
    for (size_t g = 0; g < RV.nisl; g++) { if (RV.decision[g] == 1) (*a)++; else if (RV.decision[g] == 2) (*rj)++; else (*p)++; }
}
static int edit_answers(const edit_item *e, long id) {           /* does this edit answer note `id`? */
    if (e->annotation == id) return 1;
    for (int q = 0; q < e->nextra; q++) if (e->extra[q] == id) return 1;
    return 0;
}
static void review_status_json(sb_t *r) {
    const char *st = RV.state == RV_RUNNING ? "running" : RV.state == RV_READY ? "ready" : RV.state == RV_ERROR ? "error" : "none";
    sb_printf(r, "{\"ok\":true,\"state\":\"%s\",\"backend\":\"%s\",\"scope\":\"%s\"", st, RV.backend, RV.scope);
    sb_printf(r, ",\"elapsed\":%.1f,\"dirty\":%s", RV.state == RV_NONE ? 0.0 : now_monotonic() - RV.started, RV.base_dirty ? "true" : "false");
    sb_puts(r, ",\"error\":"); if (RV.error) json_put_string(r, RV.error, strlen(RV.error)); else sb_puts(r, "null");
    sb_puts(r, ",\"raw\":"); if (RV.raw) json_put_string(r, RV.raw, strlen(RV.raw)); else sb_puts(r, "null");
    long a, rj, p; rv_counts(&a, &rj, &p);
    sb_printf(r, ",\"notes\":%zu,\"accepted\":%ld,\"rejected\":%ld,\"pending\":%ld,\"islands\":[", RV.nann, a, rj, p);
    for (size_t g = 0; g < RV.nisl; g++) {
        const island *is = &RV.isl[g];
        if (g) sb_putc(r, ',');
        sb_printf(r, "{\"n\":%zu,\"r1\":%ld,\"r2\":%ld,\"decision\":\"%s\",\"custom\":%s,\"old\":", g + 1, is->r1, is->r2, decision_name(RV.decision[g]), RV.custom[g] ? "true" : "false");
        json_put_string(r, is->old_text, strlen(is->old_text));
        const char *nt = RV.custom[g] ? RV.custom[g] : is->new_text; sb_puts(r, ",\"new\":"); json_put_string(r, nt, strlen(nt));
        sb_puts(r, ",\"edits\":[");
        for (size_t k = 0; k < is->nidx; k++) {
            const edit_item *e = &RV.edits[is->idx[k]];
            if (k) sb_putc(r, ',');
            sb_printf(r, "{\"annotation\":%ld,\"annotations\":[%ld", e->annotation, e->annotation);
            for (int q = 0; q < e->nextra; q++) sb_printf(r, ",%ld", e->extra[q]);
            sb_printf(r, "],\"also\":%s,\"why\":", e->also ? "true" : "false"); json_put_string(r, e->why, strlen(e->why)); sb_putc(r, '}');
        }
        sb_puts(r, "]}");
    }
    sb_puts(r, "],\"problems\":[");
    int first = 1;
    for (size_t i = 0; i < RV.nedits; i++) {
        const edit_item *e = &RV.edits[i]; if (e->ok) continue;
        if (!first) sb_putc(r, ',');
        first = 0;
        size_t fl = strlen(e->find); if (fl > 120) fl = 120;
        sb_printf(r, "{\"annotation\":%ld,\"find\":", e->annotation); json_put_string(r, e->find, fl);
        sb_puts(r, ",\"problem\":"); json_put_string(r, e->problem ? e->problem : "", e->problem ? strlen(e->problem) : 0); sb_putc(r, '}');
    }
    sb_puts(r, "]}");
}

/* ---- git: commit the file only when it was clean beforehand, so the author's own uncommitted edits are never swept in ---- */
static int git_ok(const char *cmd, sb_t *out) {
    char *cwd = NULL; if (H.path) { char *sl = strrchr(H.path, '/'); if (sl && sl != H.path) cwd = xstrndup(H.path, (size_t)(sl - H.path)); }
    sb_t o, e; sb_init(&o); sb_init(&e); int st = 1;
    const char *err = run_filter(cmd, cwd, "", 0, 60, &o, &e, &st);
    free(cwd);
    int ok = !err && st == 0;
    if (out) sb_put(out, o.s, o.len);
    sb_free(&o); sb_free(&e);
    return ok;
}
static int git_file_is_clean(void) {                              /* in a repository, tracked, and no uncommitted changes */
    return git_ok("git rev-parse --is-inside-work-tree >/dev/null 2>&1 && git ls-files --error-unmatch -- \"$WINLESS_FILE\" >/dev/null 2>&1 "
                  "&& test -z \"$(git status --porcelain -- \"$WINLESS_FILE\")\"", NULL);
}

static void review_finish(sb_t *r) {
    if (RV.state != RV_READY) { put_result(r, "there is no review to finish"); return; }
    if (RV.baselen != H.textlen || memcmp(RV.base, H.text, H.textlen) != 0) {
        put_result(r, "the text changed while the model was working: cancel this review (Esc) and revise again"); return;
    }
    long acc, rej, pend; rv_counts(&acc, &rej, &pend);
    sb_t out; sb_init(&out); size_t pos = 0;
    for (size_t g = 0; g < RV.nisl; g++) {
        if (RV.decision[g] != 1) continue;
        sb_put(&out, RV.base + pos, RV.isl[g].off - pos);
        const char *nt = RV.custom[g] ? RV.custom[g] : RV.isl[g].new_text; sb_puts(&out, nt);
        pos = RV.isl[g].off + RV.isl[g].len;
    }
    sb_put(&out, RV.base + pos, RV.baselen - pos);
    int changed = acc > 0 && (out.len != RV.baselen || memcmp(out.s, RV.base, RV.baselen) != 0);
    if (changed) {
        long er, ec; edit_endpos(1, 1, H.text, H.textlen, &er, &ec);
        const char *e = apply_edit(1, 1, er, ec, out.s, out.len);   /* ONE undoable edit */
        if (e) { sb_free(&out); put_result(r, e); return; }
    }
    sb_free(&out);
    /* a note is finished when its own changes were all accepted */
    sb_t done_ids; sb_init(&done_ids);
    for (size_t a = 0; a < RV.nann; a++) {
        long id = RV.ann[a]; int total = 0, taken = 0, bad = 0, primary = 0;
        for (size_t i = 0; i < RV.nedits; i++) if (edit_answers(&RV.edits[i], id) && !RV.edits[i].also) primary = 1;
        for (size_t i = 0; i < RV.nedits; i++) {
            const edit_item *e = &RV.edits[i]; if (!edit_answers(e, id) || (primary && e->also)) continue;
            total++;
            if (!e->ok) { bad = 1; continue; }
            for (size_t g = 0; g < RV.nisl; g++) for (size_t k = 0; k < RV.isl[g].nidx; k++) if (RV.isl[g].idx[k] == i) { if (RV.decision[g] == 1) taken++; else bad = 1; }
        }
        if (total > 0 && taken > 0 && !bad && store_set_status(id, "done") == NULL) sb_printf(&done_ids, "%s%ld", done_ids.len ? "," : "", id);
    }
    /* write and commit: only if the text had no unsaved edits of the author's own */
    int written = 0; char commit[24] = ""; char note[240] = "";
    if (!changed) snprintf(note, sizeof note, "nothing was accepted");
    else if (RV.base_dirty) snprintf(note, sizeof note, "the text had unsaved edits, so the result is left unsaved (write it with :w)");
    else {
        int clean = git_file_is_clean();
        const char *werr = do_write(0);
        if (werr) snprintf(note, sizeof note, "not written: %.180s", werr);
        else {
            written = 1;
            if (!clean) snprintf(note, sizeof note, "written; not committed (not a git file, or it had uncommitted changes)");
            else {
                const char *slash = strrchr(H.path, '/'); char msg[300];
                snprintf(msg, sizeof msg, "winless: revision of %s (%ld of %zu changes accepted)", slash ? slash + 1 : H.path, acc, RV.nisl);
                setenv("WINLESS_MSG", msg, 1);
                int ok = git_ok("git add -- \"$WINLESS_FILE\" && git commit -q -m \"$WINLESS_MSG\" -- \"$WINLESS_FILE\"", NULL);
                unsetenv("WINLESS_MSG");
                if (!ok) snprintf(note, sizeof note, "written; the git commit failed");
                else { sb_t h; sb_init(&h); git_ok("git rev-parse --short HEAD", &h); while (h.len && (h.s[h.len - 1] == '\n' || h.s[h.len - 1] == ' ')) h.s[--h.len] = 0; snprintf(commit, sizeof commit, "%s", h.s); sb_free(&h); snprintf(note, sizeof note, "written and committed"); }
            }
        }
    }
    store_add_review(H.path, RV.backend, getenv("WINLESS_MODEL"), RV.scope, done_ids.s, acc, rej, pend, written, commit[0] ? commit : NULL, note);
    sb_printf(r, "{\"ok\":true,\"accepted\":%ld,\"rejected\":%ld,\"pending\":%ld,\"changed\":%s,\"written\":%s,\"commit\":", acc, rej, pend, changed ? "true" : "false", written ? "true" : "false");
    if (commit[0]) json_put_string(r, commit, strlen(commit)); else sb_puts(r, "null");
    sb_puts(r, ",\"note\":"); json_put_string(r, note, strlen(note)); sb_putc(r, '}');
    sb_free(&done_ids);
    rv_clear();
    tell_pages_review();
    const char *m = "{\"cmd\":\"reload\"}"; broadcast(m, strlen(m));
}

static void review_command(const jv *v, sb_t *r) {
    const char *act = json_gets(v, "action", NULL); if (!act) act = "status";
    long n = (long)json_getnum(v, "n", 0);
    if (!strcmp(act, "status")) { review_status_json(r); return; }
    if (!strcmp(act, "cancel")) { job_kill(); rv_clear(); tell_pages_review(); put_result(r, NULL); return; }
    if (RV.state != RV_READY) { put_result(r, RV.state == RV_RUNNING ? "the model is still working" : "there is no review"); return; }
    if (!strcmp(act, "finish")) { review_finish(r); return; }
    if (!strcmp(act, "accept-all") || !strcmp(act, "reject-all")) { for (size_t g = 0; g < RV.nisl; g++) RV.decision[g] = act[0] == 'a' ? 1 : 2; tell_pages_review(); put_result(r, NULL); return; }
    if (n < 1 || (size_t)n > RV.nisl) { put_result(r, "no such change"); return; }
    size_t g = (size_t)n - 1;
    if (!strcmp(act, "accept")) RV.decision[g] = 1;
    else if (!strcmp(act, "reject")) RV.decision[g] = 2;
    else if (!strcmp(act, "edit")) {                              /* the author's own wording for this change */
        const char *t = json_gets(v, "text", NULL); if (!t) { put_result(r, "no text given"); return; }
        free(RV.custom[g]); RV.custom[g] = xstrdup(t);
    } else { put_result(r, "unknown review action"); return; }
    tell_pages_review();
    put_result(r, NULL);
}

static void edit_command(const char *cmd, const jv *v, sb_t *r) {
    if (!strcmp(cmd, "edit")) {
        size_t tl = 0; const char *tx = json_gets(v, "text", &tl);
        put_result(r, apply_edit((long)json_getnum(v, "r1", 0), (long)json_getnum(v, "c1", 0), (long)json_getnum(v, "r2", 0), (long)json_getnum(v, "c2", 0), tx, tl));
    } else if (!strcmp(cmd, "revise")) {
        revise_command(v, r);
    } else if (!strcmp(cmd, "review")) {
        review_command(v, r);
    } else if (!strcmp(cmd, "backend")) {
        const char *b = json_gets(v, "value", NULL);
        if (b && (!strcmp(b, "claude") || !strcmp(b, "api"))) { snprintf(g_backend, sizeof g_backend, "%s", b); put_result(r, NULL); }
        else put_result(r, "the backend must be claude or api");
    } else if (!strcmp(cmd, "filter")) {
        filter_command(v, r);
    } else if (!strcmp(cmd, "annotate")) {
        annotate_command(v, r);
    } else if (!strcmp(cmd, "annotations")) {
        if (!H.path && !json_getbool(v, "all", 0)) { put_result(r, "no file: the text was not opened from a file"); return; }
        sb_t an; sb_init(&an);
        const char *e = store_list_json(&an, json_getbool(v, "all", 0) ? NULL : H.path, json_getbool(v, "deleted", 0));
        if (e) put_result(r, e); else { sb_puts(r, "{\"ok\":true,\"items\":"); sb_put(r, an.s, an.len); sb_putc(r, '}'); }
        sb_free(&an);
    } else if (!strcmp(cmd, "unannotate")) {
        const char *e = store_set_deleted((long)json_getnum(v, "id", 0), !json_getbool(v, "restore", 0));
        if (!e) { const char *m = "{\"cmd\":\"reload\"}"; broadcast(m, strlen(m)); }
        put_result(r, e);
    } else if (!strcmp(cmd, "undo")) {
        if (!nundo) { put_result(r, "nothing to undo"); return; }
        undo_t u = undo_stack[nundo - 1];
        size_t nl, newlen; char *newtxt;
        char *nt = edit_replace(H.text, H.textlen, u.r1, u.c1, u.r2, u.c2, u.old, u.oldlen, &nl, &newtxt, &newlen);
        if (!nt) { put_result(r, "undo failed: the text no longer matches"); return; }
        long er, ec; edit_endpos(u.r1, u.c1, u.old, u.oldlen, &er, &ec);          /* where the restored text ends */
        stack_push(&redo_stack, &nredo, &capredo, (undo_t){ u.r1, u.c1, er, ec, newtxt, newlen });
        free(u.old); nundo--;
        free(H.text); H.text = nt; H.textlen = nl;
        text_changed(); put_result(r, NULL);
    } else if (!strcmp(cmd, "redo")) {
        if (!nredo) { put_result(r, "nothing to redo"); return; }
        undo_t e = redo_stack[nredo - 1];
        size_t nl, rl; char *restored;
        char *nt = edit_replace(H.text, H.textlen, e.r1, e.c1, e.r2, e.c2, e.old, e.oldlen, &nl, &restored, &rl);
        if (!nt) { put_result(r, "redo failed: the text no longer matches"); return; }
        long er, ec; edit_endpos(e.r1, e.c1, e.old, e.oldlen, &er, &ec);
        stack_push(&undo_stack, &nundo, &capundo, (undo_t){ e.r1, e.c1, er, ec, restored, rl });
        free(e.old); nredo--;
        free(H.text); H.text = nt; H.textlen = nl;
        text_changed(); put_result(r, NULL);
    } else if (!strcmp(cmd, "history")) {
        history_json(r);
    } else if (!strcmp(cmd, "revert")) {
        int force = json_getbool(v, "force", 0);
        if (!H.path) { put_result(r, "no file: the text was not opened from a file"); return; }
        if (is_dirty() && !force) { put_result(r, "there are unsaved edits: write them (:w) or discard them (add force) before reverting"); return; }
        char *data = NULL; size_t dl = 0; const char *err = history_read(H.path, (long)json_getnum(v, "n", 0), &data, &dl);
        if (err) { put_result(r, err); return; }
        long er, ec; edit_endpos(1, 1, H.text, H.textlen, &er, &ec);        /* the whole text, replaced as one undoable edit */
        size_t nl, rl; char *removed;
        char *nt = edit_replace(H.text, H.textlen, 1, 1, er, ec, data, dl, &nl, &removed, &rl);
        if (!nt) { free(data); put_result(r, "could not apply that version"); return; }
        long ner, nec; edit_endpos(1, 1, data, dl, &ner, &nec);
        stack_push(&undo_stack, &nundo, &capundo, (undo_t){ 1, 1, ner, nec, removed, rl }); redo_clear();
        free(H.text); H.text = nt; H.textlen = nl; free(data);
        text_changed();
        put_result(r, do_write(force));                                      /* a write: it keeps the content it replaces, so this can be reverted too */
    } else {                                         /* write */
        put_result(r, do_write(json_getbool(v, "force", 0)));
    }
}

static void h_cmd(conn *c, const char *body, size_t blen) {
    jv *v = json_parse(body, blen);
    const char *cmd = v ? json_gets(v, "cmd", NULL) : NULL;
    if (!cmd) { json_free(v); respond_err(c, 400, "bad command"); return; }
    if ((!strcmp(cmd, "load") || !strcmp(cmd, "stop")) && H.path && is_dirty() && !json_getbool(v, "force", 0)) {
        /* the text has edits that are not in the file: loading something else or stopping would throw them away */
        sb_t res; sb_init(&res);
        sb_puts(&res, "{\"ok\":false,\"error\":");
        sb_t msg; sb_init(&msg);
        sb_printf(&msg, "unsaved changes in %s (write them with :w or `winless write`, or add --force to discard them)", H.name);
        json_put_string(&res, msg.s, msg.len); sb_free(&msg);
        sb_putc(&res, '}'); json_free(v); respond_json(c, &res); return;
    }
    if (!strcmp(cmd, "edit") || !strcmp(cmd, "undo") || !strcmp(cmd, "redo") || !strcmp(cmd, "write") || !strcmp(cmd, "history") || !strcmp(cmd, "revert")
        || !strcmp(cmd, "revise") || !strcmp(cmd, "review") || !strcmp(cmd, "backend")
        || !strcmp(cmd, "filter") || !strcmp(cmd, "annotate") || !strcmp(cmd, "annotations") || !strcmp(cmd, "unannotate")) {
        sb_t res; sb_init(&res); edit_command(cmd, v, &res); json_free(v); respond_json(c, &res); return;
    }
    int stop = 0, bcast = 1;
    if (!strcmp(cmd, "load")) {
        size_t tl = 0; const char *nm = json_gets(v, "name", NULL), *tx = json_gets(v, "text", &tl);
        free(H.name); H.name = xstrdup(nm ? nm : "");
        free(H.text); H.text = xstrndup(tx ? tx : "", tl); H.textlen = tl;
        H.version++; clear_marks(); clear_hls();
        { const char *pth = json_gets(v, "path", NULL); free(H.path); H.path = pth ? xstrdup(pth) : NULL;
          H.mtime = H.path ? file_mtime_ns(H.path) : -1; undo_clear(); }
        free(H.search); H.search = NULL; set_str(&H.search, json_get(v, "search"));
        H.top = (long)json_getnum(v, "line", 1); if (H.top < 1) H.top = 1;
        apply_settings(json_get(v, "settings"));
        sb_t b; sb_init(&b); sb_puts(&b, "{\"cmd\":\"load\"}");
        broadcast(b.s, b.len); sb_free(&b); bcast = 0;             /* the page re-fetches /content: no text over SSE */
    } else if (!strcmp(cmd, "config")) apply_settings(v);
    else if (!strcmp(cmd, "mark")) {
        H.marks = xrealloc(H.marks, (H.nmarks + 1) * sizeof *H.marks);
        H.marks[H.nmarks].a = (long)json_getnum(v, "a", 1); H.marks[H.nmarks].b = (long)json_getnum(v, "b", 1);
        H.marks[H.nmarks].color = NULL; set_str(&H.marks[H.nmarks].color, json_get(v, "color")); H.nmarks++;
    } else if (!strcmp(cmd, "unmark")) clear_marks();
    else if (!strcmp(cmd, "hl")) {
        const char *p = json_gets(v, "pattern", NULL);
        if (p) {
            H.hls = xrealloc(H.hls, (H.nhls + 1) * sizeof *H.hls);
            H.hls[H.nhls].pattern = xstrdup(p); H.hls[H.nhls].color = NULL; set_str(&H.hls[H.nhls].color, json_get(v, "color")); H.nhls++;
        }
    } else if (!strcmp(cmd, "unhl")) clear_hls();
    else if (!strcmp(cmd, "search")) { free(H.search); H.search = NULL; set_str(&H.search, json_get(v, "pattern")); }
    else if (!strcmp(cmd, "clear")) { free(H.search); H.search = NULL; }
    else if (!strcmp(cmd, "stop")) { stop = 1; bcast = 0; }
    if (bcast) { sb_t b; sb_init(&b); json_write(&b, v); broadcast(b.s, b.len); sb_free(&b); }
    json_free(v);
    sb_t r; sb_init(&r);
    sb_printf(&r, "{\"ok\":true,\"clients\":%zu,\"seq\":%ld}", sse_count(), H.seq);
    respond_json(c, &r);
    if (stop) g_stop = 1;
}

static void handle_request(conn *c, const char *method, const char *path, const char *body, size_t blen) {
    char *query = strchr((char *)path, '?');
    char pathbuf[256];
    size_t pl = query ? (size_t)(query - path) : strlen(path);
    if (pl >= sizeof pathbuf) { respond_err(c, 404, "not found"); return; }
    memcpy(pathbuf, path, pl); pathbuf[pl] = 0;
    const char *q = query ? query + 1 : "";
    int get = !strcmp(method, "GET"), post = !strcmp(method, "POST");
    if (get && !strcmp(pathbuf, "/")) h_index(c);
    else if (get && !strcmp(pathbuf, "/ping")) {
        sb_t b; sb_init(&b); sb_puts(&b, "{\"ok\":true,\"version\":\"" PACKAGE_VERSION "\"}"); respond_json(c, &b);
    } else if (get && !strcmp(pathbuf, "/content")) h_content(c);
    else if (get && !strcmp(pathbuf, "/status")) h_status(c);
    else if (get && !strcmp(pathbuf, "/events")) h_events(c);
    else if (get && !strcmp(pathbuf, "/wait")) {
        double since = 0, timeout = 3600;
        qparam(q, "since", &since); qparam(q, "timeout", &timeout);
        c->wait_since = (long)since; c->wait_deadline = now_monotonic() + timeout; c->state = ST_WAIT;
        if (H.seq > c->wait_since) wait_reply(c);
    } else if (post && !strcmp(pathbuf, "/event")) h_event(c, body, blen);
    else if (post && !strcmp(pathbuf, "/cmd")) h_cmd(c, body, blen);
    else respond_err(c, 404, "not found");
}

/* Only this machine's own pages may talk to the server: a web page from anywhere else (or a hostname rebound to 127.0.0.1) must not be
 * able to edit, write or run a filter.  Browsers always send Host, and Origin on cross-origin and POST requests. */
static int local_name_ok(const char *h, int with_scheme) {
    if (with_scheme) { if (strncasecmp(h, "http://", 7) != 0) return 0; h += 7; }
    size_t n;
    if (*h == '[') { const char *e = strchr(h, ']'); if (!e) return 0; n = (size_t)(e - h) + 1; }
    else { const char *e = strchr(h, ':'); n = e ? (size_t)(e - h) : strlen(h); }
    int ok = (n == 9 && !strncasecmp(h, "localhost", 9)) || (n == 9 && !strncmp(h, "127.0.0.1", 9)) || (n == 5 && !strncmp(h, "[::1]", 5));
    if (!ok) return 0;
    if (h[n] == ':') { char *end; long p = strtol(h + n + 1, &end, 10); return *end == 0 && p == g_listen_port; }
    return h[n] == 0;
}
static int request_allowed(const char *host, const char *origin) {
    if (host && !local_name_ok(host, 0)) return 0;
    if (origin && !local_name_ok(origin, 1)) return 0;
    return 1;
}

/* Try to parse one complete request out of c->in. */
static void try_request(conn *c) {
    char *hdr_end = strstr(c->in.s, "\r\n\r\n");
    if (!hdr_end) { if (c->in.len > 65536) { respond_err(c, 400, "headers too large"); } return; }
    size_t head_len = (size_t)(hdr_end - c->in.s) + 4;
    char *line_end = strstr(c->in.s, "\r\n");
    char method[16] = "", path[2048] = "";
    if (sscanf(c->in.s, "%15s %2047s", method, path) != 2 || !line_end) { respond_err(c, 400, "bad request"); return; }
    size_t clen = 0; char host[200] = "", origin[200] = ""; int have_host = 0, have_origin = 0;
    for (char *p = line_end + 2; p < hdr_end + 2;) {
        char *e = strstr(p, "\r\n"); if (!e) break;
        if (!strncasecmp(p, "content-length:", 15)) { unsigned long long v = strtoull(p + 15, NULL, 10); if (v > MAX_BODY) { respond_err(c, 400, "body too large"); return; } clen = (size_t)v; }
        else if (!strncasecmp(p, "host:", 5) || !strncasecmp(p, "origin:", 7)) {
            int is_host = !strncasecmp(p, "host:", 5); char *val = p + (is_host ? 5 : 7);
            while (val < e && (*val == ' ' || *val == '\t')) val++;
            size_t vl = (size_t)(e - val); char *dst = is_host ? host : origin; if (vl >= 200) vl = 199;
            memcpy(dst, val, vl); dst[vl] = 0; if (is_host) have_host = 1; else have_origin = 1;
        }
        p = e + 2;
    }
    if (!request_allowed(have_host ? host : NULL, have_origin ? origin : NULL)) { respond_err(c, 403, "forbidden: only pages from this machine may use this server"); return; }
    if (c->in.len < head_len + clen) return;                  /* body not complete yet */
    handle_request(c, method, path, c->in.s + head_len, clen);
}

/* ---------------------------------------------------------------------------------------------------- loop */
static int make_listener(int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0 || listen(s, 64) < 0) { close(s); return -1; }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
    return s;
}

/* Every server records its port in ~/.cache/winless/ports/PORT (its pid inside), so `winless windows` can find them all. */
static void registry_file(int port, char *buf, size_t n) {
    char dir[1024]; winless_cache_dir(dir, sizeof dir);
    char pd[1100]; snprintf(pd, sizeof pd, "%s/ports", dir); mkdir(pd, 0755);
    snprintf(buf, n, "%s/%d", pd, port);
}
static void registry_add(int port) {
    char f[1200]; registry_file(port, f, sizeof f);
    FILE *fp = fopen(f, "w"); if (fp) { fprintf(fp, "%ld\n", (long)getpid()); fclose(fp); }
}
static void registry_remove(int port) { char f[1200]; registry_file(port, f, sizeof f); unlink(f); }

int server_run(int port, double idle_secs) {
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term; sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    hub_init();
    int ls = make_listener(port);
    if (ls < 0) { fprintf(stderr, "winless: cannot listen on 127.0.0.1:%d: %s\n", port, strerror(errno)); return 1; }
    printf("winless %s serving on http://127.0.0.1:%d/\n", PACKAGE_VERSION, port); fflush(stdout);
    registry_add(port);
    g_idle_secs = idle_secs; g_listen_port = port;
    double last_busy = now_monotonic();                             /* for idle_secs: the last time a window was connected or edits were unsaved */

    while (!g_stop) {
        struct pollfd *pf = xmalloc((nconns + 1) * sizeof *pf);
        pf[0].fd = ls; pf[0].events = POLLIN; pf[0].revents = 0;
        for (size_t i = 0; i < nconns; i++) {
            pf[i + 1].fd = conns[i]->fd; pf[i + 1].revents = 0;
            pf[i + 1].events = (short)((conns[i]->state != ST_CLOSING ? POLLIN : 0) | (conns[i]->outpos < conns[i]->out.len ? POLLOUT : 0));
        }
        size_t n_polled = nconns;                                   /* only these have a pollfd slot */
        int rc = poll(pf, (nfds_t)(nconns + 1), g_job.active ? 100 : 500);
        if (rc < 0 && errno != EINTR) { free(pf); break; }
        if (rc > 0 && (pf[0].revents & POLLIN)) {
            int fd;
            while ((fd = accept(ls, NULL, NULL)) >= 0) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); conn_new(fd); }
        }
        for (size_t i = 0; i < n_polled; i++) {
            conn *c = conns[i];
            if (c->fd < 0 || rc <= 0) continue;
            short re = pf[i + 1].revents;
            if (re & POLLIN) {
                char buf[65536];
                ssize_t n = read(c->fd, buf, sizeof buf);
                if (n > 0) {
                    if (c->state == ST_REQ) { sb_put(&c->in, buf, (size_t)n); try_request(c); }
                    /* SSE / WAIT: the client should send nothing more; discard it */
                } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) { conn_close(c); continue; }
            } else if (re & (POLLERR | POLLHUP | POLLNVAL)) { conn_close(c); continue; }
            if (c->fd >= 0) flush_conn(c);
        }
        free(pf);
        job_pump();
        double now = now_monotonic();
        if (sse_count() > 0 || g_job.active) last_busy = now;
        else if (idle_secs > 0 && now - last_busy > idle_secs) g_stop = 1;       /* its window is gone: unsaved edits go with it */
        if (g_quit_at > 0 && now >= g_quit_at) g_stop = 1;
        for (size_t i = 0; i < nconns; i++) {
            conn *c = conns[i];
            if (c->fd < 0) continue;
            if (c->state == ST_WAIT && (H.seq > c->wait_since || now >= c->wait_deadline)) { wait_reply(c); flush_conn(c); }
            else if (c->state == ST_SSE && now - c->last_write > SSE_PING_SECS) { sb_puts(&c->out, ": ping\n\n"); flush_conn(c); }
        }
        compact();
    }
    for (size_t i = 0; i < nconns; i++) flush_conn(conns[i]);       /* let the reply to a `stop` command out */
    for (size_t i = 0; i < nconns; i++) conn_close(conns[i]);
    job_kill();
    compact();
    close(ls);
    registry_remove(port);
    return 0;
}
