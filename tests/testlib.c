/* testlib.c -- test harness and black-box helpers.  Copyright (C) 2026 David Walther.  GPL-3.0-or-later. */
#define _POSIX_C_SOURCE 200809L
#include "testlib.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

const char *g_bin;
int g_port;
static pid_t g_pid;
static char g_tmp[512];
static char g_errfile[600];
static jmp_buf g_jmp;
static char g_failmsg[4096];

/* open sockets owned by the current test, closed when it ends */
#define MAX_FDS 64
static int g_fds[MAX_FDS];
static void fd_track(int fd) { for (int i = 0; i < MAX_FDS; i++) if (g_fds[i] < 0) { g_fds[i] = fd; return; } }
static void fd_untrack(int fd) { for (int i = 0; i < MAX_FDS; i++) if (g_fds[i] == fd) g_fds[i] = -1; }
static void fd_closeall(void) { for (int i = 0; i < MAX_FDS; i++) if (g_fds[i] >= 0) { close(g_fds[i]); g_fds[i] = -1; } }

void msleep(int ms) { struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }

/* ----------------------------------------------------------------------------------------------- harness */
void test_fail(const char *file, int line, const char *fmt, ...) {
    int n = snprintf(g_failmsg, sizeof g_failmsg, "%s:%d: ", file, line);
    va_list ap; va_start(ap, fmt);
    vsnprintf(g_failmsg + n, sizeof g_failmsg - (size_t)n, fmt, ap);
    va_end(ap);
    longjmp(g_jmp, 1);
}

/* Servers that a test started by itself (winless open, without --port) are recorded in the registry: stop them all. */
static void kill_registered_servers(void) {
    char pd[1400]; snprintf(pd, sizeof pd, "%s/home/.cache/winless/ports", g_tmp);
    DIR *d = opendir(pd); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char f[2000]; snprintf(f, sizeof f, "%s/%s", pd, e->d_name);
        long pid = 0; FILE *fp = fopen(f, "r"); if (fp) { if (fscanf(fp, "%ld", &pid) != 1) pid = 0; fclose(fp); }
        if (pid > 1) { kill((pid_t)pid, SIGKILL); waitpid((pid_t)pid, NULL, WNOHANG); }
        unlink(f);
    }
    closedir(d);
}

/* One test: (start server,) run, close sockets, (stop server). The setjmp lives here so the caller's loop is not clobbered. */
static int run_one(const testcase *t) {
    volatile int ok = 1;
    g_failmsg[0] = 0;
    if (t->needs_server) {
        if (setjmp(g_jmp) == 0) server_start(); else ok = 0;
    }
    if (ok) { if (setjmp(g_jmp) == 0) t->fn(); else ok = 0; }
    fd_closeall();
    if (t->needs_server) {
        char saved[sizeof g_failmsg]; memcpy(saved, g_failmsg, sizeof saved);
        if (setjmp(g_jmp) != 0) { if (ok) ok = 0; else memcpy(g_failmsg, saved, sizeof saved); }   /* a failure at stop time (sanitizer output) */
        else server_stop();
    }
    kill_registered_servers();                 /* after the harness server has been stopped: it is registered too */
    return ok;
}

int run_tests(const testcase *t, size_t n) {
    int fails = 0;
    for (int i = 0; i < MAX_FDS; i++) g_fds[i] = -1;
    for (size_t i = 0; i < n; i++) {
        int ok = run_one(&t[i]);
        printf("%s %s\n", ok ? "ok  " : "FAIL", t[i].name);
        if (!ok) { printf("     %s\n", g_failmsg); fails++; }
        fflush(stdout);
    }
    printf("%zu test(s), %d failed\n", n, fails);
    return fails;
}

/* ---------------------------------------------------------------------------------------------- environment */
static void init_common(void) {
    signal(SIGPIPE, SIG_IGN);
    for (int i = 0; i < MAX_FDS; i++) g_fds[i] = -1;
    const char *base = getenv("TMPDIR"); if (!base || !*base) base = "/tmp";
    snprintf(g_tmp, sizeof g_tmp, "%s/winless-test-XXXXXX", base);
    if (!mkdtemp(g_tmp)) { perror("mkdtemp"); exit(1); }
    char home[640]; snprintf(home, sizeof home, "%s/home", g_tmp);
    mkdir(home, 0755);
    setenv("HOME", home, 1);                /* the server log of auto-started servers goes here, not into the real ~ */
    unsetenv("XDG_CACHE_HOME");
    unsetenv("XDG_DATA_HOME");
    unsetenv("WINLESS_PORT");
    unsetenv("WINLESS_BROWSER");
    { char dp[16]; snprintf(dp, sizeof dp, "%d", free_port()); setenv("WINLESS_DEFAULT_PORT", dp, 1); }   /* tests never look at the real port 8777 */
}

void testlib_init_noserver(void) { init_common(); }

void testlib_init(void) {
    g_bin = getenv("WINLESS_BIN");
    if (!g_bin || !*g_bin) g_bin = "./winless";
    if (access(g_bin, X_OK) != 0) { fprintf(stderr, "tests: cannot execute %s (build it, or set WINLESS_BIN)\n", g_bin); exit(77); }
    init_common();
}

const char *tmp_dir(void) { return g_tmp; }

char *tmp_write(const char *name, const char *data, size_t len) {
    static char path[8][700];
    static int k;
    char *p = path[k++ % 8];
    snprintf(p, 700, "%s/%s", g_tmp, name);
    FILE *f = fopen(p, "wb");
    if (!f) FAIL("cannot write %s", p);
    fwrite(data, 1, len, f);
    fclose(f);
    return p;
}

int free_port(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) { perror("bind"); exit(1); }
    socklen_t l = sizeof a; getsockname(s, (struct sockaddr *)&a, &l);
    int port = ntohs(a.sin_port);
    close(s);
    return port;
}

/* ----------------------------------------------------------------------------------------------- the server */
static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9; }

static int ping_ok(void) {
    resp_t r = http("GET", "/ping", NULL, 0);
    int ok = r.status == 200 && r.body && strstr(r.body, "\"ok\":true");
    free(r.body);
    return ok;
}

void server_start(void) {
    g_port = free_port();
    snprintf(g_errfile, sizeof g_errfile, "%s/server.err", g_tmp);
    char port[16]; snprintf(port, sizeof port, "%d", g_port);
    g_pid = fork();
    if (g_pid < 0) FAIL("fork failed");
    if (g_pid == 0) {
        int err = open(g_errfile, O_WRONLY | O_CREAT | O_TRUNC, 0644), nul = open("/dev/null", O_WRONLY);
        if (err >= 0) dup2(err, 2);
        if (nul >= 0) dup2(nul, 1);
        execl(g_bin, g_bin, "--port", port, "serve", (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < 100; i++) {
        if (waitpid(g_pid, NULL, WNOHANG) == g_pid) { g_pid = 0; FAIL("the server exited at startup (port %d)", g_port); }
        if (ping_ok()) return;
        msleep(50);
    }
    kill(g_pid, SIGKILL); waitpid(g_pid, NULL, 0); g_pid = 0;
    FAIL("the server did not answer on port %d", g_port);
}

int server_exited(double timeout) {
    double end = now() + timeout;
    for (;;) {
        if (g_pid == 0) return 1;
        if (waitpid(g_pid, NULL, WNOHANG) == g_pid) { g_pid = 0; return 1; }
        if (now() >= end) return 0;
        msleep(20);
    }
}

void server_stop(void) {
    if (g_pid) {
        kill(g_pid, SIGTERM);
        if (!server_exited(5.0)) { kill(g_pid, SIGKILL); waitpid(g_pid, NULL, 0); g_pid = 0; FAIL("the server ignored SIGTERM"); }
    }
    size_t n = 0;
    char *err = NULL;
    FILE *f = fopen(g_errfile, "rb");
    if (f) { sb_t b; sb_init(&b); char buf[4096]; size_t k; while ((k = fread(buf, 1, sizeof buf, f)) > 0) sb_put(&b, buf, k); fclose(f); err = sb_detach(&b, &n); }
    if (err && (strstr(err, "AddressSanitizer") || strstr(err, "runtime error") || strstr(err, "LeakSanitizer")))
        FAIL("the server reported a sanitizer/undefined-behaviour error:\n%.1500s", err);
    free(err);
    remove(g_errfile);
}

/* -------------------------------------------------------------------------------------------------- sockets */
static int connect_server(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)g_port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof a) < 0) { if (s >= 0) close(s); return -1; }
    return s;
}

static int send_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t k = write(fd, p, n);                       /* write(), not send(): also used on pipes (SIGPIPE is ignored) */
        if (k < 0) { if (errno == EINTR) continue; return -1; }
        p += k; n -= (size_t)k;
    }
    return 0;
}

/* Read until EOF or the deadline; returns malloc'ed data. */
static char *read_until_close(int fd, double timeout, size_t *len) {
    sb_t b; sb_init(&b);
    double end = now() + timeout;
    for (;;) {
        double left = end - now();
        if (left <= 0) break;
        struct pollfd pf = { fd, POLLIN, 0 };
        int r = poll(&pf, 1, (int)(left * 1000) + 1);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        char buf[65536];
        ssize_t n = read(fd, buf, sizeof buf);            /* read(), not recv(): this is also used on pipes */
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        sb_put(&b, buf, (size_t)n);
    }
    return sb_detach(&b, len);
}

resp_t http(const char *method, const char *path, const char *body, size_t blen) {
    resp_t r = { 0, NULL, 0 };
    int s = connect_server();
    if (s < 0) return r;
    sb_t req; sb_init(&req);
    sb_printf(&req, "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n", method, path, blen);
    if (blen) sb_put(&req, body, blen);
    int sent = send_all(s, req.s, req.len);
    sb_free(&req);
    if (sent == 0) {
        size_t n; char *in = read_until_close(s, 60.0, &n);
        char *sep = strstr(in, "\r\n\r\n");
        int code = 0;
        if (sep && sscanf(in, "HTTP/%*s %d", &code) == 1) {
            size_t hl = (size_t)(sep - in) + 4;
            r.status = code; r.len = n - hl; r.body = xstrndup(in + hl, r.len);
        }
        free(in);
    }
    close(s);
    return r;
}

char *get_ok(const char *path) {
    resp_t r = http("GET", path, NULL, 0);
    if (r.status != 200) FAIL("GET %s -> HTTP %d", path, r.status);
    return r.body;
}
char *post_ok(const char *path, const char *body) {
    resp_t r = http("POST", path, body, strlen(body));
    if (r.status != 200) FAIL("POST %s -> HTTP %d [%.200s]", path, r.status, r.body ? r.body : "");
    return r.body;
}
int post_status(const char *path, const char *body) {
    resp_t r = http("POST", path, body, strlen(body));
    free(r.body);
    return r.status;
}
jv *get_json(const char *path) {
    char *b = get_ok(path);
    jv *j = json_parse(b, strlen(b));
    if (!j) FAIL("GET %s returned invalid JSON: %.200s", path, b);
    free(b);
    return j;
}
long clients(void) { jv *j = get_json("/status"); long n = (long)json_getnum(j, "clients", -1); json_free(j); return n; }
void wait_clients(long n) {
    for (int i = 0; i < 100; i++) { if (clients() == n) return; msleep(50); }
    FAIL("clients never became %ld (it is %ld)", n, clients());
}

/* ------------------------------------------------------------------------------------------------------ SSE */
struct sse { int fd; sb_t buf; int headers_done; char **ev; size_t n, cap; };

static void sse_parse(sse_t *s) {
    if (!s->headers_done) {
        char *sep = strstr(s->buf.s, "\r\n\r\n");
        if (!sep) return;
        size_t cut = (size_t)(sep - s->buf.s) + 4;
        memmove(s->buf.s, s->buf.s + cut, s->buf.len - cut + 1); s->buf.len -= cut;
        s->headers_done = 1;
    }
    char *nl;
    while ((nl = strchr(s->buf.s, '\n'))) {
        size_t ll = (size_t)(nl - s->buf.s);
        if (ll > 6 && !strncmp(s->buf.s, "data: ", 6)) {
            if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 16; s->ev = xrealloc(s->ev, s->cap * sizeof *s->ev); }
            s->ev[s->n++] = xstrndup(s->buf.s + 6, ll - 6);
        }
        memmove(s->buf.s, nl + 1, s->buf.len - ll); s->buf.len -= ll + 1;
    }
}

static void sse_pump(sse_t *s, double timeout) {
    struct pollfd pf = { s->fd, POLLIN, 0 };
    if (poll(&pf, 1, (int)(timeout * 1000)) > 0) {
        char buf[65536];
        ssize_t n = recv(s->fd, buf, sizeof buf, 0);
        if (n > 0) { sb_put(&s->buf, buf, (size_t)n); sse_parse(s); }
    }
}

sse_t *sse_open(void) {
    int fd = connect_server();
    if (fd < 0) FAIL("cannot connect to the server");
    const char *req = "GET /events HTTP/1.1\r\nHost: 127.0.0.1\r\nAccept: text/event-stream\r\n\r\n";
    if (send_all(fd, req, strlen(req))) FAIL("cannot send the events request");
    sse_t *s = xmalloc(sizeof *s); memset(s, 0, sizeof *s);
    s->fd = fd; sb_init(&s->buf);
    fd_track(fd);
    double end = now() + 5;
    while (!s->headers_done && now() < end) sse_pump(s, 0.1);
    if (!s->headers_done) FAIL("the event stream did not open");
    return s;
}

size_t sse_wait(sse_t *s, size_t n, double timeout) {
    double end = now() + timeout;
    while (s->n < n && now() < end) sse_pump(s, 0.05);
    return s->n;
}

const char *sse_event(sse_t *s, size_t i) { return i < s->n ? s->ev[i] : NULL; }

void sse_close(sse_t *s) {
    if (s->fd >= 0) { shutdown(s->fd, SHUT_RDWR); close(s->fd); fd_untrack(s->fd); s->fd = -1; }
}

/* ------------------------------------------------------------------------------------------------- raw sockets */
int raw_connect(void) { int fd = connect_server(); if (fd < 0) FAIL("cannot connect to the server"); fd_track(fd); return fd; }
void raw_send(int fd, const char *data, size_t len) { send_all(fd, data, len); }
char *raw_read(int fd, double timeout, size_t *len) { return read_until_close(fd, timeout, len); }
void raw_close(int fd) { close(fd); fd_untrack(fd); }

/* ------------------------------------------------------------------------------------------------ the command line */
pid_t cli_spawn(char *const args[], int *out_fd) {
    int outp[2];
    if (pipe(outp) < 0) FAIL("pipe failed");
    char port[16]; snprintf(port, sizeof port, "%d", g_port);
    size_t na = 0; while (args[na]) na++;
    char **argv = xmalloc((na + 4) * sizeof *argv);
    argv[0] = (char *)g_bin; argv[1] = "--port"; argv[2] = port;
    for (size_t i = 0; i < na; i++) argv[3 + i] = args[i];
    argv[3 + na] = NULL;
    pid_t pid = fork();
    if (pid < 0) FAIL("fork failed");
    if (pid == 0) {
        dup2(outp[1], 1); close(outp[0]); close(outp[1]);
        execv(g_bin, argv);
        _exit(127);
    }
    close(outp[1]);
    free(argv);
    *out_fd = outp[0];
    return pid;
}

run_t cli_finish(pid_t pid, int out_fd, double timeout) {
    run_t r = { -1, NULL, 0 };
    r.out = read_until_close(out_fd, timeout, &r.outlen);
    close(out_fd);
    int st = 0;
    double end = now() + timeout;
    while (waitpid(pid, &st, WNOHANG) == 0) {
        if (now() > end) { kill(pid, SIGKILL); waitpid(pid, &st, 0); FAIL("the command did not finish within %.0f s", timeout); }
        msleep(10);
    }
    r.code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    return r;
}

/* Like cli_run, but without --port: the command finds the servers itself (and has no terminal to ask on). */
run_t cli_auto(char *const args[]) {
    int outp[2];
    if (pipe(outp) < 0) FAIL("pipe failed");
    size_t na = 0; while (args[na]) na++;
    char **argv = xmalloc((na + 2) * sizeof *argv);
    argv[0] = (char *)g_bin;
    for (size_t i = 0; i < na; i++) argv[1 + i] = args[i];
    argv[1 + na] = NULL;
    pid_t pid = fork();
    if (pid < 0) FAIL("fork failed");
    if (pid == 0) {
        int nul = open("/dev/null", O_RDONLY); if (nul >= 0) dup2(nul, 0);
        dup2(outp[1], 1); close(outp[0]); close(outp[1]);
        execv(g_bin, argv);
        _exit(127);
    }
    free(argv);
    close(outp[1]);
    return cli_finish(pid, outp[0], 60.0);
}

run_t cli_run(const char *input, size_t inlen, char *const args[]) {
    int outp[2], inp[2];
    if (pipe(outp) < 0 || pipe(inp) < 0) FAIL("pipe failed");
    char port[16]; snprintf(port, sizeof port, "%d", g_port);
    size_t na = 0; while (args[na]) na++;
    char **argv = xmalloc((na + 4) * sizeof *argv);
    argv[0] = (char *)g_bin; argv[1] = "--port"; argv[2] = port;
    for (size_t i = 0; i < na; i++) argv[3 + i] = args[i];
    argv[3 + na] = NULL;
    pid_t pid = fork();
    if (pid < 0) FAIL("fork failed");
    if (pid == 0) {
        dup2(outp[1], 1); dup2(inp[0], 0);
        close(outp[0]); close(outp[1]); close(inp[0]); close(inp[1]);
        execv(g_bin, argv);
        _exit(127);
    }
    free(argv);
    close(outp[1]); close(inp[0]);
    if (input && inlen) send_all(inp[1], input, inlen);       /* small inputs only: fits the pipe buffer or the child reads */
    close(inp[1]);
    return cli_finish(pid, outp[0], 60.0);
}
