/* server.c -- the winless HTTP/SSE server: one thread, one poll() loop.
 * Copyright (C) 2026 David Walther.  GPL-3.0-or-later.
 *
 * Endpoints:  GET /  /ping  /content  /status  /wait  /events (Server-Sent Events)    POST /event  /cmd
 */
#define _POSIX_C_SOURCE 200809L
#include "winless.h"
#include <arpa/inet.h>
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
} H;

static conn **conns;
static size_t nconns, capconns;
static volatile sig_atomic_t g_stop;

static void on_term(int sig) { (void)sig; g_stop = 1; }

/* ------------------------------------------------------------------------------------------------------- hub */
static void hub_init(void) {
    memset(&H, 0, sizeof H);
    H.name = xstrdup(""); H.text = xstrdup(""); H.numbers = H.wrap = -1; H.top = 1;
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
    const char *reason = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 404 ? "Not Found" : "Internal Server Error";
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
    sb_printf(&b, ",\"version\":%ld,\"settings\":", H.version); settings_json(&b);
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
    sb_puts(&b, "],\"search\":"); put_opt_string(&b, H.search);
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
    }
    json_free(v);
    respond(c, 200, "application/json; charset=utf-8", "{\"ok\":true}", 11);
}

static void h_cmd(conn *c, const char *body, size_t blen) {
    jv *v = json_parse(body, blen);
    const char *cmd = v ? json_gets(v, "cmd", NULL) : NULL;
    if (!cmd) { json_free(v); respond_err(c, 400, "bad command"); return; }
    int stop = 0, bcast = 1;
    if (!strcmp(cmd, "load")) {
        size_t tl = 0; const char *nm = json_gets(v, "name", NULL), *tx = json_gets(v, "text", &tl);
        free(H.name); H.name = xstrdup(nm ? nm : "");
        free(H.text); H.text = xstrndup(tx ? tx : "", tl); H.textlen = tl;
        H.version++; clear_marks(); clear_hls();
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

/* Try to parse one complete request out of c->in. */
static void try_request(conn *c) {
    char *hdr_end = strstr(c->in.s, "\r\n\r\n");
    if (!hdr_end) { if (c->in.len > 65536) { respond_err(c, 400, "headers too large"); } return; }
    size_t head_len = (size_t)(hdr_end - c->in.s) + 4;
    char *line_end = strstr(c->in.s, "\r\n");
    char method[16] = "", path[2048] = "";
    if (sscanf(c->in.s, "%15s %2047s", method, path) != 2 || !line_end) { respond_err(c, 400, "bad request"); return; }
    size_t clen = 0;
    for (char *p = line_end + 2; p < hdr_end + 2;) {
        char *e = strstr(p, "\r\n"); if (!e) break;
        if (!strncasecmp(p, "content-length:", 15)) { unsigned long long v = strtoull(p + 15, NULL, 10); if (v > MAX_BODY) { respond_err(c, 400, "body too large"); return; } clen = (size_t)v; }
        p = e + 2;
    }
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

int server_run(int port) {
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term; sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    hub_init();
    int ls = make_listener(port);
    if (ls < 0) { fprintf(stderr, "winless: cannot listen on 127.0.0.1:%d: %s\n", port, strerror(errno)); return 1; }
    printf("winless %s serving on http://127.0.0.1:%d/\n", PACKAGE_VERSION, port); fflush(stdout);

    while (!g_stop) {
        struct pollfd *pf = xmalloc((nconns + 1) * sizeof *pf);
        pf[0].fd = ls; pf[0].events = POLLIN; pf[0].revents = 0;
        for (size_t i = 0; i < nconns; i++) {
            pf[i + 1].fd = conns[i]->fd; pf[i + 1].revents = 0;
            pf[i + 1].events = (short)((conns[i]->state != ST_CLOSING ? POLLIN : 0) | (conns[i]->outpos < conns[i]->out.len ? POLLOUT : 0));
        }
        size_t n_polled = nconns;                                   /* only these have a pollfd slot */
        int rc = poll(pf, (nfds_t)(nconns + 1), 500);
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
        double now = now_monotonic();
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
    compact();
    close(ls);
    return 0;
}
