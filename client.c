/* client.c -- the winless command line: talks to the server, starts it on demand, opens the window.
 * Copyright (C) 2026 David Walther.  GPL-3.0-or-later. */
#define _POSIX_C_SOURCE 200809L
#include "winless.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void msleep(int ms) { struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }

/* ------------------------------------------------------------------------------------------------ HTTP client */
/* Returns the HTTP status (0 on a transport failure); *resp is the body (malloc'ed, NUL-terminated). */
static int http_do(int port, const char *method, const char *path, const char *body, size_t blen, double timeout, char **resp, size_t *rlen) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 0;
    struct timeval tv; tv.tv_sec = (time_t)timeout; tv.tv_usec = (suseconds_t)((timeout - (double)(time_t)timeout) * 1e6);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) { close(s); return 0; }
    sb_t req; sb_init(&req);
    sb_printf(&req, "%s %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n",
              method, path, port, blen);
    if (blen) sb_put(&req, body, blen);
    size_t off = 0;
    while (off < req.len) {
        ssize_t n = send(s, req.s + off, req.len - off, MSG_NOSIGNAL);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; sb_free(&req); close(s); return 0; }
        off += (size_t)n;
    }
    sb_free(&req);
    sb_t in; sb_init(&in);
    char buf[65536]; ssize_t n;
    while ((n = recv(s, buf, sizeof buf, 0)) != 0) {
        if (n < 0) { if (errno == EINTR) continue; break; }
        sb_put(&in, buf, (size_t)n);
    }
    close(s);
    int code = 0;
    char *sep = strstr(in.s, "\r\n\r\n");
    if (in.len >= 12 && sep && sscanf(in.s, "HTTP/%*s %d", &code) == 1) {
        size_t hl = (size_t)(sep - in.s) + 4;
        if (resp) { *resp = xstrndup(in.s + hl, in.len - hl); if (rlen) *rlen = in.len - hl; }
    } else code = 0;
    sb_free(&in);
    return code;
}

int server_alive(int port) {
    char *r = NULL;
    int code = http_do(port, "GET", "/ping", NULL, 0, 1.0, &r, NULL);
    free(r);
    return code == 200;
}

static char *self_path(void) {
    static char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return NULL;
    buf[n] = 0;
    return buf;
}

static int spawn_detached(char *const argv[], int use_path, const char *logfile) {
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        setsid();
        int in = open("/dev/null", O_RDONLY);
        int out = logfile ? open(logfile, O_WRONLY | O_CREAT | O_APPEND, 0644) : open("/dev/null", O_WRONLY);
        if (in >= 0) dup2(in, 0);
        if (out >= 0) { dup2(out, 1); dup2(out, 2); }
        if (use_path) execvp(argv[0], argv); else execv(argv[0], argv);
        _exit(127);
    }
    return 1;
}

static void ensure_server(int port, double idle_secs) {
    if (server_alive(port)) return;
    char dir[1024], log[1100];
    winless_cache_dir(dir, sizeof dir);
    snprintf(log, sizeof log, "%s/server.log", dir);
    char pbuf[16]; snprintf(pbuf, sizeof pbuf, "%d", port);
    char ibuf[16]; snprintf(ibuf, sizeof ibuf, "%d", (int)idle_secs);
    char *me = self_path();
    char *argv[8]; int n = 0;
    argv[n++] = me; argv[n++] = "--port"; argv[n++] = pbuf; argv[n++] = "serve";
    if (idle_secs > 0) { argv[n++] = "--exit-when-idle"; argv[n++] = ibuf; }
    argv[n] = NULL;
    if (!me || !spawn_detached(argv, 0, log)) { fprintf(stderr, "winless: cannot start the server\n"); exit(1); }
    for (int i = 0; i < 50; i++) { if (server_alive(port)) return; msleep(100); }
    fprintf(stderr, "winless: the server did not start (see %s)\n", log);
    exit(1);
}

/* POST a command (a JSON object body) to the server. */
static int send_cmd(int port, sb_t *json, char **resp) {
    ensure_server(port, 0);
    int code = http_do(port, "POST", "/cmd", json->s, json->len, 15.0, resp, NULL);
    sb_free(json);
    if (code != 200) { fprintf(stderr, "winless: the server did not accept the command (HTTP %d)\n", code); return 1; }
    return 0;
}

static void jkey(sb_t *b, const char *k) { sb_printf(b, ",\"%s\":", k); }
static void jopt(sb_t *b, const char *k, const char *v) { jkey(b, k); if (v) json_put_string(b, v, strlen(v)); else sb_puts(b, "null"); }

static int simple_cmd(int port, const char *cmd) {
    sb_t b; sb_init(&b); sb_printf(&b, "{\"cmd\":\"%s\"}", cmd);
    return send_cmd(port, &b, NULL);
}

/* --------------------------------------------------------------------------------------------- the window */
static const char *BROWSERS[] = {
    "/mnt/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe",
    "/mnt/c/Program Files/Microsoft/Edge/Application/msedge.exe",
    "/mnt/c/Program Files/Google/Chrome/Application/chrome.exe",
    "/mnt/c/Program Files (x86)/Google/Chrome/Application/chrome.exe",
    "/mnt/c/Program Files/BraveSoftware/Brave-Browser/Application/brave.exe",
    NULL
};

static const char *find_browser(void) {
    const char *env = getenv("WINLESS_BROWSER");
    if (env && *env) return env;
    for (int i = 0; BROWSERS[i]; i++) if (access(BROWSERS[i], X_OK) == 0) return BROWSERS[i];
    return NULL;
}

/* Edge/Chrome hand a new window to an already-running browser process and then ignore flags such as --start-fullscreen.
 * A dedicated profile directory forces a separate process that honours them (and keeps winless out of the user's own
 * browser profile).  The directory is %LOCALAPPDATA%\\winless\\profile; $WINLESS_PROFILE overrides it (empty = none). */
static char *windows_profile_dir(void) {
    const char *env = getenv("WINLESS_PROFILE");
    if (env) return *env ? xstrdup(env) : NULL;
    FILE *p = popen("cd /mnt/c 2>/dev/null && cmd.exe /c \"echo %LOCALAPPDATA%\" 2>/dev/null", "r");
    if (!p) return NULL;
    char line[1024] = "";
    char *got = fgets(line, sizeof line, p);
    pclose(p);
    if (!got) return NULL;
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (n < 3 || line[1] != ':') return NULL;                       /* not a Windows path (the variable was not expanded) */
    sb_t b; sb_init(&b);
    sb_puts(&b, line); sb_puts(&b, "\\winless\\profile");
    return sb_detach(&b, NULL);
}

static int open_window(int port, int windowed) {
    char url[64], app[96];
    snprintf(url, sizeof url, "http://localhost:%d/", port);
    snprintf(app, sizeof app, "--app=%s", url);
    const char *b = find_browser();
    if (b) {
        char *argv[16]; int n = 0;
        argv[n++] = (char *)b; argv[n++] = app;
        argv[n++] = windowed ? "--window-size=1100,820" : "--start-fullscreen";
        argv[n++] = "--new-window"; argv[n++] = "--no-first-run"; argv[n++] = "--no-default-browser-check";
        /* keep painting and running timers while another window covers this one (the window is driven from the shell) */
        argv[n++] = "--disable-features=CalculateNativeWinOcclusion"; argv[n++] = "--disable-backgrounding-occluded-windows";
        argv[n++] = "--disable-background-timer-throttling"; argv[n++] = "--disable-renderer-backgrounding";
        char *prof = NULL, *udd = NULL;
        size_t bl = strlen(b);
        if (bl > 4 && !strcasecmp(b + bl - 4, ".exe") && (prof = windows_profile_dir()) != NULL) {
            sb_t u; sb_init(&u); sb_puts(&u, "--user-data-dir="); sb_puts(&u, prof);
            udd = sb_detach(&u, NULL);
            argv[n++] = udd;
        }
        argv[n] = NULL;
        int ok = spawn_detached(argv, 0, NULL);
        free(prof); free(udd);
        return ok;
    }
    char *ex[] = { "explorer.exe", url, NULL };
    if (spawn_detached(ex, 1, NULL)) return 1;
    char *xo[] = { "xdg-open", url, NULL };
    return spawn_detached(xo, 1, NULL);
}

static long status_num(int port, const char *key) {          /* a top-level number from /status */
    char *r = NULL; size_t rl = 0;
    long v = 0;
    if (http_do(port, "GET", "/status", NULL, 0, 10.0, &r, &rl) == 200) {
        jv *j = json_parse(r, rl); v = (long)json_getnum(j, key, 0); json_free(j);
    }
    free(r);
    return v;
}

static int ensure_window(int port, int windowed) {
    if (status_num(port, "clients") > 0) return 1;
    if (!open_window(port, windowed)) { fprintf(stderr, "winless: no Windows browser found; open http://localhost:%d/ yourself\n", port); return 0; }
    for (int i = 0; i < 100; i++) { if (status_num(port, "clients") > 0) return 1; msleep(200); }
    fprintf(stderr, "winless: the window did not connect (is http://localhost:%d/ reachable from Windows?)\n", port);
    return 0;
}

/* -------------------------------------------------------------------------------------------------- commands */
static void usage(FILE *f) {
    fputs("usage: winless [--port N] COMMAND [ARGS]\n"
          "  open [FILE|-] [--new|--here] [--force] [--name N] [--line N] [--search P] [--theme dark|light] [--font F] [--size PX]\n"
          "       [-N] [-S] [--windowed] [--no-window] [--wait]     show a file (or stdin)\n"
          "  goto LINE|N%|top|bottom      scroll [-p] N      search [-b] PATTERN   next  prev  clear\n"
          "  mark A[-B] [COLOR]  unmark   hl PATTERN [COLOR]  unhl   key KEY...   flash TEXT\n"
          "  config [--theme T] [--font F] [--size N] [--numbers|--no-numbers] [--wrap|--chop]\n"
          "  status [--json] [--text]     wait [--timeout S]     window [--windowed]   close\n"
          "  history | revert [N]    (every write keeps the old file; revert puts one back)\n"
          "  annotate R1 C1 R2 C2 NOTE | --file NOTE    annotations [--all]    unannotate [--restore] ID\n"
          "  filter R1 C1 R2 C2 [--lines] COMMAND       (replace a range with the output of a shell command)\n"
          "  windows   (list the open windows)    stop [--all] [--force]    serve | start      --version   --help\n"
          "Each open starts its own server and window; with several open, commands ask which (or use --port N).\n"
          "See winless(1).\n", f);
}

static const struct { const char *name, *key; } KEYNAMES[] = {
    {"space", " "}, {"enter", "Enter"}, {"return", "Enter"}, {"up", "ArrowUp"}, {"down", "ArrowDown"}, {"pgup", "PageUp"},
    {"pgdn", "PageDown"}, {"pageup", "PageUp"}, {"pagedown", "PageDown"}, {"home", "Home"}, {"end", "End"},
    {"esc", "Escape"}, {"escape", "Escape"}, {NULL, NULL}
};

static int is_num(const char *s) { char *e; if (!*s) return 0; strtod(s, &e); return !*e; }

static char *read_stdin(size_t *len) {
    sb_t b; sb_init(&b);
    char buf[65536]; ssize_t n;
    while ((n = read(0, buf, sizeof buf)) > 0) sb_put(&b, buf, (size_t)n);
    return sb_detach(&b, len);
}


/* ---- finding the running servers: each window is its own server, on a port chosen when it was opened ---- */
static int default_port(void) { const char *e = getenv("WINLESS_DEFAULT_PORT"); int p = e && *e ? atoi(e) : 0; return p > 0 ? p : DEFAULT_PORT; }

static int free_local_port(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0); if (s < 0) return 0;
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) { close(s); return 0; }
    socklen_t l = sizeof a; getsockname(s, (struct sockaddr *)&a, &l);
    int port = ntohs(a.sin_port); close(s); return port;
}

typedef struct { int port; long clients; int dirty; char *name, *path; } sinfo;
static void sinfo_free(sinfo *s) { free(s->name); free(s->path); s->name = s->path = NULL; }
static int server_info(int port, sinfo *si) {
    memset(si, 0, sizeof *si); si->port = port;
    char *r = NULL; size_t rl = 0;
    if (http_do(port, "GET", "/content", NULL, 0, 5.0, &r, &rl) != 200) { free(r); return 0; }
    jv *j = json_parse(r, rl); free(r);
    if (!j) return 0;
    const char *n = json_gets(j, "name", NULL), *p = json_gets(j, "path", NULL);
    si->name = xstrdup(n ? n : ""); si->path = p ? xstrdup(p) : NULL; si->dirty = json_getbool(j, "dirty", 0);
    json_free(j);
    si->clients = status_num(port, "clients");
    return 1;
}
static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static int list_servers(int *ports, int max) {          /* the live servers: the registry, plus the default port (old servers keep no record) */
    int n = 0;
    char dir[1024], pd[1100]; winless_cache_dir(dir, sizeof dir); snprintf(pd, sizeof pd, "%s/ports", dir);
    DIR *d = opendir(pd);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && n < max) {
            int p = atoi(e->d_name); if (p < 1 || p > 65535) continue;
            if (server_alive(p)) ports[n++] = p;
            else { char f[1400]; snprintf(f, sizeof f, "%s/%s", pd, e->d_name); unlink(f); }      /* a server that is gone */
        }
        closedir(d);
    }
    int dp = default_port(), have = 0;
    for (int i = 0; i < n; i++) if (ports[i] == dp) have = 1;
    if (!have && n < max && server_alive(dp)) ports[n++] = dp;
    qsort(ports, (size_t)n, sizeof *ports, cmp_int);
    return n;
}
static void print_windows(FILE *f, const int *ports, int n, int numbered) {
    for (int i = 0; i < n; i++) {
        sinfo si;
        if (numbered) fprintf(f, "  %d) ", i + 1); else fprintf(f, "  ");
        if (!server_info(ports[i], &si)) { fprintf(f, "port %d  (not answering)\n", ports[i]); continue; }
        fprintf(f, "port %-5d %-8s %s%s", ports[i], si.clients > 0 ? "window" : "no window", si.name && *si.name ? si.name : "(no name)", si.dirty ? " [unsaved]" : "");
        if (si.path) fprintf(f, "   %s", si.path);
        fputc('\n', f); sinfo_free(&si);
    }
}
/* Which server should a command act on?  One: that one.  Several: do not guess -- ask on a terminal, otherwise list them.
 * Returns the port, 0 if there is none, -1 if the choice was not made. */
static int resolve_port(void) {
    int ports[64], n = list_servers(ports, 64);
    if (n == 1) return ports[0];
    if (n == 0) { fprintf(stderr, "winless: no window is open (start one with `winless open FILE`)\n"); return 0; }
    if (isatty(0) && isatty(2)) {
        fprintf(stderr, "Several windows are open:\n"); print_windows(stderr, ports, n, 1);
        fprintf(stderr, "Which one? [1-%d] ", n);
        char line[64]; if (!fgets(line, sizeof line, stdin)) return -1;
        int k = atoi(line); if (k >= 1 && k <= n) return ports[k - 1];
        fprintf(stderr, "winless: no such window\n"); return -1;
    }
    fprintf(stderr, "winless: %d windows are open; choose one with --port N:\n", n); print_windows(stderr, ports, n, 0);
    return -1;
}

static int cmd_open(int port, int explicit, int argc, char **argv) {
    const char *file = NULL, *name = NULL, *search = NULL, *theme = NULL, *font = NULL;
    long line = 0, size = 0;
    int numbers = 0, chop = 0, wait = 0, nowin = 0, windowed = 0, force = 0, newwin = 0, here = 0;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(a, "--line") && i + 1 < argc) line = atol(argv[++i]);
        else if (!strcmp(a, "--search") && i + 1 < argc) search = argv[++i];
        else if (!strcmp(a, "--theme") && i + 1 < argc) theme = argv[++i];
        else if (!strcmp(a, "--font") && i + 1 < argc) font = argv[++i];
        else if (!strcmp(a, "--size") && i + 1 < argc) size = atol(argv[++i]);
        else if (!strcmp(a, "-N") || !strcmp(a, "--numbers")) numbers = 1;
        else if (!strcmp(a, "-S") || !strcmp(a, "--chop")) chop = 1;
        else if (!strcmp(a, "--wait")) wait = 1;
        else if (!strcmp(a, "--force")) force = 1;
        else if (!strcmp(a, "--new")) newwin = 1;
        else if (!strcmp(a, "--here")) here = 1;
        else if (!strcmp(a, "--no-window")) nowin = 1;
        else if (!strcmp(a, "--windowed")) windowed = 1;
        else if (!strcmp(a, "-") && !file) file = a;
        else if (a[0] != '-' && !file) file = a;
        else { fprintf(stderr, "winless open: unknown argument %s\n", a); return 2; }
    }
    /* Opening must never disturb a window that is already open: unless told otherwise, every open starts its own server,
     * on a port chosen now, and that server serves only this window.  --here replaces the text of the one existing window. */
    int fresh = 0;
    if (!explicit) {
        if (here) { int p = resolve_port(); if (p <= 0) return p == 0 ? 1 : 2; port = p; }
        else {
            char *rp0 = (file && strcmp(file, "-")) ? realpath(file, NULL) : NULL;
            if (rp0 && !newwin) {
                int ports[64], n = list_servers(ports, 64);
                for (int i = 0; i < n; i++) {
                    sinfo si;
                    if (server_info(ports[i], &si)) {
                        int same = si.path && !strcmp(si.path, rp0);
                        sinfo_free(&si);
                        if (same) { printf("already open on port %d (see `winless windows`); use --new for another window\n", ports[i]); free(rp0); return 0; }
                    }
                }
            }
            free(rp0);
            port = free_local_port();
            if (!port) { fprintf(stderr, "winless open: cannot find a free port\n"); return 1; }
            fresh = 1;
        }
    }
    ensure_server(port, fresh && !nowin ? 60.0 : 0);              /* a server made for a window quits once the window is gone and nothing is unsaved */
    char *data; size_t dlen;
    if (!file || !strcmp(file, "-")) {
        if (!file && isatty(0)) { fprintf(stderr, "winless open: give a FILE, or pipe text in\n"); return 2; }
        data = read_stdin(&dlen); if (!name) name = "(stdin)";
    } else {
        data = read_file(file, &dlen);
        if (!data) { fprintf(stderr, "winless open: cannot read %s: %s\n", file, strerror(errno)); return 1; }
        if (!name) { const char *sl = strrchr(file, '/'); name = sl ? sl + 1 : file; }
    }
    long since = status_num(port, "seq");
    sb_t clean; sb_init(&clean); sb_put_utf8_clean(&clean, data, dlen); free(data);
    sb_t b; sb_init(&b);
    sb_puts(&b, "{\"cmd\":\"load\""); if (force) sb_puts(&b, ",\"force\":true");
    jopt(&b, "name", name);
    jkey(&b, "text"); json_put_string(&b, clean.s, clean.len); sb_free(&clean);
    { char *rp = (file && strcmp(file, "-")) ? realpath(file, NULL) : NULL; jopt(&b, "path", rp); free(rp); }
    jopt(&b, "search", search);
    jkey(&b, "line"); if (line > 0) sb_printf(&b, "%ld", line); else sb_puts(&b, "null");
    sb_puts(&b, ",\"settings\":{");
    int first = 1;
    if (theme) { sb_puts(&b, "\"theme\":"); json_put_string(&b, theme, strlen(theme)); first = 0; }
    if (font) { if (!first) sb_putc(&b, ','); sb_puts(&b, "\"font\":"); json_put_string(&b, font, strlen(font)); first = 0; }
    if (size > 0) { if (!first) sb_putc(&b, ','); sb_printf(&b, "\"size\":%ld", size); first = 0; }
    if (numbers) { if (!first) sb_putc(&b, ','); sb_puts(&b, "\"numbers\":true"); first = 0; }
    if (chop) { if (!first) sb_putc(&b, ','); sb_puts(&b, "\"wrap\":false"); }
    sb_puts(&b, "}}");
    char *lresp = NULL;
    if (send_cmd(port, &b, &lresp)) return 1;
    { jv *lj = json_parse(lresp ? lresp : "", lresp ? strlen(lresp) : 0); free(lresp);
      if (lj && !json_getbool(lj, "ok", 1)) { const char *e = json_gets(lj, "error", NULL); fprintf(stderr, "winless open: %s\n", e ? e : "refused"); json_free(lj); return 1; }
      json_free(lj); }
    if (!nowin) ensure_window(port, windowed);
    if (fresh) printf("opened on port %d\n", port);
    if (wait) {
        char path[96]; snprintf(path, sizeof path, "/wait?since=%ld&timeout=86400", since);
        char *r = NULL; size_t rl = 0;
        int code = http_do(port, "GET", path, NULL, 0, 86400.0 + 30, &r, &rl);
        int timed_out = 1;
        if (code == 200) { jv *j = json_parse(r, rl); timed_out = json_getbool(j, "timeout", 1); json_free(j); }
        free(r);
        return timed_out ? 1 : 0;
    }
    return 0;
}

/* winless edit R1 C1 R2 C2 TEXT|-   replace the range [R1:C1, R2:C2) (rows from 1, columns in code points from 1, end excluded)
 * winless undo                       undo the last edit
 * winless redo                       redo the last undone edit
 * winless write [-f]                 write the text to its file (-f: even if the file changed on disk) */
static int cmd_edit(int port, const char *cmd, int argc, char **argv) {
    sb_t b; sb_init(&b); sb_printf(&b, "{\"cmd\":\"%s\"", cmd);
    if (!strcmp(cmd, "edit")) {
        if (argc != 5) { sb_free(&b); fprintf(stderr, "usage: winless edit R1 C1 R2 C2 TEXT|-\n"); return 2; }
        const char *names[4] = { "r1", "c1", "r2", "c2" };
        for (int i = 0; i < 4; i++) { jkey(&b, names[i]); json_put_number(&b, atof(argv[i])); }
        char *txt = NULL; size_t tl = 0;
        if (!strcmp(argv[4], "-")) txt = read_stdin(&tl); else { txt = xstrdup(argv[4]); tl = strlen(txt); }
        sb_t clean; sb_init(&clean); sb_put_utf8_clean(&clean, txt, tl); free(txt);
        jkey(&b, "text"); json_put_string(&b, clean.s, clean.len); sb_free(&clean);
    } else if (!strcmp(cmd, "write") && argc > 0 && !strcmp(argv[0], "-f")) sb_puts(&b, ",\"force\":true");
    else if (!strcmp(cmd, "revert")) {                      /* revert [N] [--force]: put back version N (default the latest) of the file */
        for (int i = 0; i < argc; i++) {
            if (!strcmp(argv[i], "--force") || !strcmp(argv[i], "-f")) sb_puts(&b, ",\"force\":true");
            else if (is_num(argv[i])) { jkey(&b, "n"); json_put_number(&b, atof(argv[i])); }
            else { sb_free(&b); fprintf(stderr, "usage: winless revert [N] [--force]\n"); return 2; }
        }
    }
    sb_putc(&b, '}');
    char *resp = NULL;
    if (send_cmd(port, &b, &resp)) return 1;
    jv *j = json_parse(resp ? resp : "", resp ? strlen(resp) : 0); free(resp);
    int ok = json_getbool(j, "ok", 0);
    if (!ok) { const char *e = json_gets(j, "error", NULL); fprintf(stderr, "winless %s: %s\n", cmd, e ? e : "failed"); }
    json_free(j);
    return ok ? 0 : 1;
}

/* winless history: the earlier versions of the file that writes have kept, oldest first (winless revert N puts one back) */
static int cmd_history(int port) {
    sb_t b; sb_init(&b); sb_puts(&b, "{\"cmd\":\"history\"}");
    char *resp = NULL;
    if (send_cmd(port, &b, &resp)) return 1;
    jv *j = json_parse(resp ? resp : "", resp ? strlen(resp) : 0); free(resp);
    if (!j || !json_getbool(j, "ok", 0)) { const char *e = j ? json_gets(j, "error", NULL) : NULL; fprintf(stderr, "winless history: %s\n", e ? e : "failed"); json_free(j); return 1; }
    const jv *items = json_get(j, "items");
    printf("%s\n", json_gets(j, "path", NULL));
    if (!items || items->t != J_ARR || items->n == 0) puts("  (no earlier versions kept yet)");
    else for (size_t i = 0; i < items->n; i++) {
        const jv *it = items->items[i]; const char *t = json_gets(it, "time", NULL);
        char when[40] = "";
        if (t && strlen(t) >= 16) snprintf(when, sizeof when, "%.4s-%.2s-%.2s %.2s:%.2s:%.2s UTC", t, t + 4, t + 6, t + 9, t + 11, t + 13);
        printf("  %4.0f  %s  %.0f bytes\n", json_getnum(it, "n", 0), when, json_getnum(it, "bytes", 0));
    }
    json_free(j);
    return 0;
}

/* ---- annotations and filters (the work is done by the server; these are the command-line doors to it) ---- */
static jv *ask(int port, sb_t *b, const char *what) {         /* send a command, parse the reply, print its error; NULL on failure */
    char *resp = NULL;
    if (send_cmd(port, b, &resp)) return NULL;
    jv *j = json_parse(resp ? resp : "", resp ? strlen(resp) : 0); free(resp);
    if (!j || !json_getbool(j, "ok", 0)) { const char *e = j ? json_gets(j, "error", NULL) : NULL; fprintf(stderr, "winless %s: %s\n", what, e ? e : "failed"); json_free(j); return NULL; }
    return j;
}
/* annotate R1 C1 R2 C2 NOTE   (rows and columns from 1, the end excluded -- as for edit)      annotate --file NOTE   (the whole file) */
static int cmd_annotate(int port, int argc, char **argv) {
    sb_t b; sb_init(&b); sb_puts(&b, "{\"cmd\":\"annotate\"");
    const char *note;
    if (argc == 2 && !strcmp(argv[0], "--file")) { sb_puts(&b, ",\"file\":true"); note = argv[1]; }
    else if (argc == 5) {
        const char *names[4] = { "r1", "c1", "r2", "c2" };
        for (int i = 0; i < 4; i++) { jkey(&b, names[i]); json_put_number(&b, atof(argv[i])); }
        note = argv[4];
    } else { sb_free(&b); fprintf(stderr, "usage: winless annotate R1 C1 R2 C2 NOTE | annotate --file NOTE\n"); return 2; }
    sb_t clean; sb_init(&clean); sb_put_utf8_clean(&clean, note, strlen(note));
    jopt(&b, "note", clean.s); sb_free(&clean); sb_putc(&b, '}');
    jv *j = ask(port, &b, "annotate"); if (!j) return 1;
    printf("note %.0f saved (%s)\n", json_getnum(j, "id", 0), json_gets(j, "scope", NULL)); json_free(j);
    return 0;
}
/* annotations [--all] [--deleted]: the notes on this file (or on every file) */
static int cmd_annotations(int port, int argc, char **argv) {
    sb_t b; sb_init(&b); sb_puts(&b, "{\"cmd\":\"annotations\"");
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--all")) sb_puts(&b, ",\"all\":true"); else if (!strcmp(argv[i], "--deleted")) sb_puts(&b, ",\"deleted\":true");
        else { sb_free(&b); fprintf(stderr, "usage: winless annotations [--all] [--deleted]\n"); return 2; }
    }
    sb_putc(&b, '}');
    jv *j = ask(port, &b, "annotations"); if (!j) return 1;
    const jv *items = json_get(j, "items");
    if (!items || items->n == 0) puts("no notes");
    else for (size_t i = 0; i < items->n; i++) {
        const jv *it = items->items[i]; const char *scope = json_gets(it, "scope", NULL), *note = json_gets(it, "note", NULL), *text = json_gets(it, "text", NULL), *paras = json_gets(it, "paragraphs", NULL);
        char where[80];
        if (scope && !strcmp(scope, "text")) snprintf(where, sizeof where, "%.0f:%.0f-%.0f:%.0f", json_getnum(it, "r1", 0), json_getnum(it, "c1", 0), json_getnum(it, "r2", 0), json_getnum(it, "c2", 0));
        else if (scope && !strcmp(scope, "paragraphs")) snprintf(where, sizeof where, "paragraphs %s", paras ? paras : "?");
        else snprintf(where, sizeof where, "whole file");
        printf("#%.0f  %s%s  %s  %s\n", json_getnum(it, "id", 0), json_getbool(it, "deleted", 0) ? "[hidden] " : "", json_gets(it, "created", NULL), where, note ? note : "");
        if (text) printf("      on: %s\n", text);
    }
    json_free(j);
    return 0;
}
/* unannotate [--restore] ID: hide a note (nothing is deleted from the database), or bring it back */
static int cmd_unannotate(int port, int argc, char **argv) {
    int restore = argc > 0 && !strcmp(argv[0], "--restore");
    if (argc != 1 + restore || !is_num(argv[restore])) { fprintf(stderr, "usage: winless unannotate [--restore] ID\n"); return 2; }
    sb_t b; sb_init(&b); sb_puts(&b, "{\"cmd\":\"unannotate\""); jkey(&b, "id"); json_put_number(&b, atof(argv[restore])); if (restore) sb_puts(&b, ",\"restore\":true"); sb_putc(&b, '}');
    jv *j = ask(port, &b, "unannotate"); if (!j) return 1; json_free(j);
    return 0;
}
/* filter R1 C1 R2 C2 [--lines] [--timeout S] COMMAND: replace the range with the output of a shell command run on it (one undoable edit) */
static int cmd_filter(int port, int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: winless filter R1 C1 R2 C2 [--lines] [--timeout S] COMMAND\n"); return 2; }
    sb_t b; sb_init(&b); sb_puts(&b, "{\"cmd\":\"filter\"");
    const char *names[4] = { "r1", "c1", "r2", "c2" };
    for (int i = 0; i < 4; i++) { jkey(&b, names[i]); json_put_number(&b, atof(argv[i])); }
    int i = 4;
    for (; i < argc - 1; i++) {
        if (!strcmp(argv[i], "--lines")) sb_puts(&b, ",\"linewise\":true");
        else if (!strcmp(argv[i], "--timeout") && i + 1 < argc - 1) { jkey(&b, "timeout"); json_put_number(&b, atof(argv[++i])); }
        else break;
    }
    if (i != argc - 1) { sb_free(&b); fprintf(stderr, "usage: winless filter R1 C1 R2 C2 [--lines] [--timeout S] COMMAND\n"); return 2; }
    jopt(&b, "command", argv[i]); sb_putc(&b, '}');
    jv *j = ask(port, &b, "filter"); if (!j) return 1; json_free(j);
    return 0;
}

static int cmd_status(int port, int argc, char **argv) {
    int json = 0, text = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) json = 1; else if (!strcmp(argv[i], "--text")) text = 1;
        else { fprintf(stderr, "winless status: unknown argument %s\n", argv[i]); return 2; }
    }
    ensure_server(port, 0);
    char *r = NULL; size_t rl = 0;
    if (http_do(port, "GET", "/status", NULL, 0, 10.0, &r, &rl) != 200) { fprintf(stderr, "winless: no status\n"); free(r); return 1; }
    if (json) { printf("%s\n", r); free(r); return 0; }
    jv *j = json_parse(r, rl); free(r);
    const jv *v = json_get(j, "view");
    if (json_getnum(j, "clients", 0) < 1) { puts("no window connected"); json_free(j); return 0; }
    const char *name = json_gets(v, "name", NULL); if (!name) name = json_gets(j, "name", NULL);
    double total = json_getnum(v, "total", 0), bottom = json_getnum(v, "bottom", 0);
    int pct = total > 0 ? (int)(100.0 * bottom / total + 0.5) : 0;
    printf("%s: lines %.0f-%.0f of %.0f (%d%%)%s\n", name ? name : "", json_getnum(v, "top", 0), bottom, total, pct,
           json_getbool(v, "atEnd", 0) ? "  [END]" : "");
    const char *search = json_gets(v, "search", NULL);
    if (search) printf("search /%s: %.0f line(s); current line %.0f\n", search, json_getnum(v, "matchLines", 0), json_getnum(v, "curLine", 0));
    const char *theme = json_gets(v, "theme", NULL);
    printf("window %.0fx%.0fpx  numbers=%s wrap=%s theme=%s\n", json_getnum(v, "width", 0), json_getnum(v, "height", 0),
           json_getbool(v, "numbers", 0) ? "true" : "false", json_getbool(v, "wrap", 0) ? "true" : "false", theme ? theme : "");
    if (text) {
        puts("--- visible ---");
        const jv *vis = json_get(v, "visible");
        if (vis && vis->t == J_ARR) for (size_t i = 0; i < vis->n; i++) if (vis->items[i]->t == J_STR) printf("%s\n", vis->items[i]->str);
    }
    json_free(j);
    return 0;
}

static int cmd_wait(int port, int argc, char **argv) {
    double timeout = 86400;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--timeout") && i + 1 < argc) timeout = atof(argv[++i]);
        else { fprintf(stderr, "winless wait: unknown argument %s\n", argv[i]); return 2; }
    }
    ensure_server(port, 0);
    long since = status_num(port, "seq");
    char path[128]; snprintf(path, sizeof path, "/wait?since=%ld&timeout=%g", since, timeout);
    char *r = NULL; size_t rl = 0;
    int code = http_do(port, "GET", path, NULL, 0, timeout + 30, &r, &rl);
    if (code != 200) { fprintf(stderr, "winless wait: no answer from the server\n"); free(r); return 1; }
    jv *j = json_parse(r, rl); free(r);
    int to = json_getbool(j, "timeout", 1);
    const char *ev = json_gets(j, "event", NULL);
    puts(to || !ev ? "timeout" : ev);
    json_free(j);
    return to ? 1 : 0;
}

int client_main(int port, int explicit_port, int argc, char **argv) {
    if (argc < 1) { usage(stderr); return 2; }
    const char *cmd = argv[0];
    int ac = argc - 1; char **av = argv + 1;
    sb_t b;

    if (!strcmp(cmd, "help")) { usage(stdout); return 0; }
    int stop_all = !strcmp(cmd, "stop") && ac > 0 && (!strcmp(av[0], "--all") || (ac > 1 && !strcmp(av[1], "--all")));
    if (!strcmp(cmd, "windows") || !strcmp(cmd, "list")) {
        int ports[64], n = list_servers(ports, 64);
        if (!n) { puts("no windows open"); return 0; }
        print_windows(stdout, ports, n, 0);
        return 0;
    }
    if (!explicit_port) {
        int own = !strcmp(cmd, "open") || stop_all;
        if (!strcmp(cmd, "serve") || !strcmp(cmd, "start")) port = default_port();
        else if (!own) { int p = resolve_port(); if (p == 0) return 1; if (p < 0) return 2; port = p; }
    }
    if (!strcmp(cmd, "serve")) {
        double idle = 0;
        for (int i = 0; i + 1 < ac; i++) if (!strcmp(av[i], "--exit-when-idle")) idle = atof(av[i + 1]);
        return server_run(port, idle);
    }
    if (!strcmp(cmd, "start")) { ensure_server(port, 0); printf("winless server on port %d\n", port); return 0; }
    if (!strcmp(cmd, "stop")) {
        int force = 0; for (int i = 0; i < ac; i++) if (!strcmp(av[i], "--force")) force = 1;
        int ports[64], n = 0, refused = 0;
        if (stop_all) n = list_servers(ports, 64); else { ports[0] = port; n = 1; }
        for (int i = 0; i < n; i++) {
            if (!server_alive(ports[i])) continue;
            char *r = NULL; size_t rl = 0;
            const char *body = force ? "{\"cmd\":\"stop\",\"force\":true}" : "{\"cmd\":\"stop\"}";
            http_do(ports[i], "POST", "/cmd", body, strlen(body), 5.0, &r, &rl);
            jv *j = json_parse(r ? r : "", r ? rl : 0); free(r);
            if (j && !json_getbool(j, "ok", 1)) { const char *e = json_gets(j, "error", NULL); fprintf(stderr, "winless stop (port %d): %s\n", ports[i], e ? e : "refused"); refused = 1; }
            json_free(j);
        }
        return refused ? 1 : 0;
    }
    if (!strcmp(cmd, "open")) return cmd_open(port, explicit_port, ac, av);
    if (!strcmp(cmd, "status")) return cmd_status(port, ac, av);
    if (!strcmp(cmd, "history")) return cmd_history(port);
    if (!strcmp(cmd, "annotate")) return cmd_annotate(port, ac, av);
    if (!strcmp(cmd, "annotations")) return cmd_annotations(port, ac, av);
    if (!strcmp(cmd, "unannotate")) return cmd_unannotate(port, ac, av);
    if (!strcmp(cmd, "filter")) return cmd_filter(port, ac, av);
    if (!strcmp(cmd, "edit") || !strcmp(cmd, "undo") || !strcmp(cmd, "redo") || !strcmp(cmd, "write") || !strcmp(cmd, "revert")) return cmd_edit(port, cmd, ac, av);
    if (!strcmp(cmd, "wait")) return cmd_wait(port, ac, av);
    if (!strcmp(cmd, "window")) {
        int windowed = ac > 0 && !strcmp(av[0], "--windowed");
        ensure_server(port, 0);
        return ensure_window(port, windowed) ? 0 : 1;
    }
    if (!strcmp(cmd, "type")) {                            /* winless type [--commit] TEXT: fill the open bottom panel (and apply it) */
        int commit = ac > 0 && !strcmp(av[0], "--commit");
        if (ac < 1 + commit) { fprintf(stderr, "usage: winless type [--commit] TEXT\n"); return 2; }
        sb_init(&b); sb_puts(&b, "{\"cmd\":\"type\""); jopt(&b, "text", av[commit]); if (commit) sb_puts(&b, ",\"commit\":true"); sb_putc(&b, '}');
        return send_cmd(port, &b, NULL);
    }
    if (!strcmp(cmd, "ex")) {                              /* winless ex COMMAND: run an ex command (s/a/b/g, 5, w ...) in the window */
        if (ac < 1) { fprintf(stderr, "usage: winless ex COMMAND\n"); return 2; }
        sb_init(&b); sb_puts(&b, "{\"cmd\":\"ex\""); jopt(&b, "text", av[0]); sb_putc(&b, '}');
        return send_cmd(port, &b, NULL);
    }
    if (!strcmp(cmd, "goto")) {
        if (ac != 1) { fprintf(stderr, "usage: winless goto LINE|N%%|top|bottom\n"); return 2; }
        sb_init(&b); sb_puts(&b, "{\"cmd\":\"goto\"");
        size_t l = strlen(av[0]);
        if (!strcasecmp(av[0], "top") || !strcasecmp(av[0], "bottom")) { for (char *p = av[0]; *p; p++) *p = (char)((*p >= 'A' && *p <= 'Z') ? *p + 32 : *p); jopt(&b, "where", av[0]); }
        else if (l > 1 && av[0][l - 1] == '%') { jkey(&b, "percent"); json_put_number(&b, atof(av[0])); }
        else if (is_num(av[0])) { jkey(&b, "line"); json_put_number(&b, atof(av[0])); }
        else { sb_free(&b); fprintf(stderr, "winless goto: not a line, percentage, top or bottom: %s\n", av[0]); return 2; }
        sb_putc(&b, '}');
        return send_cmd(port, &b, NULL);
    }
    if (!strcmp(cmd, "scroll")) {
        int page = 0; const char *n = NULL;
        for (int i = 0; i < ac; i++) { if (!strcmp(av[i], "-p") || !strcmp(av[i], "--page")) page = 1; else if (is_num(av[i]) && !n) n = av[i]; else { fprintf(stderr, "usage: winless scroll [-p] N\n"); return 2; } }
        if (!n) { fprintf(stderr, "usage: winless scroll [-p] N\n"); return 2; }
        sb_init(&b); sb_puts(&b, "{\"cmd\":\"scroll\",\"n\":"); json_put_number(&b, atof(n)); sb_printf(&b, ",\"page\":%s}", page ? "true" : "false");
        return send_cmd(port, &b, NULL);
    }
    if (!strcmp(cmd, "search")) {
        int back = 0; const char *pat = NULL;
        for (int i = 0; i < ac; i++) { if (!strcmp(av[i], "-b") || !strcmp(av[i], "--backward")) back = 1; else if (!pat) pat = av[i]; else { fprintf(stderr, "usage: winless search [-b] PATTERN\n"); return 2; } }
        if (!pat) { fprintf(stderr, "usage: winless search [-b] PATTERN\n"); return 2; }
        sb_init(&b); sb_puts(&b, "{\"cmd\":\"search\""); jopt(&b, "pattern", pat); sb_printf(&b, ",\"backward\":%s}", back ? "true" : "false");
        return send_cmd(port, &b, NULL);
    }
    if (!strcmp(cmd, "mark")) {
        if (ac < 1 || ac > 2) { fprintf(stderr, "usage: winless mark A[-B] [COLOR]\n"); return 2; }
        long lo = atol(av[0]), hi = lo; const char *dash = strchr(av[0], '-');
        if (dash && dash[1]) hi = atol(dash + 1);
        sb_init(&b); sb_printf(&b, "{\"cmd\":\"mark\",\"a\":%ld,\"b\":%ld", lo, hi); jopt(&b, "color", ac > 1 ? av[1] : NULL); sb_putc(&b, '}');
        return send_cmd(port, &b, NULL);
    }
    if (!strcmp(cmd, "hl")) {
        if (ac < 1 || ac > 2) { fprintf(stderr, "usage: winless hl PATTERN [COLOR]\n"); return 2; }
        sb_init(&b); sb_puts(&b, "{\"cmd\":\"hl\""); jopt(&b, "pattern", av[0]); jopt(&b, "color", ac > 1 ? av[1] : NULL); sb_putc(&b, '}');
        return send_cmd(port, &b, NULL);
    }
    if (!strcmp(cmd, "key")) {
        if (ac < 1) { fprintf(stderr, "usage: winless key KEY...\n"); return 2; }
        sb_init(&b); sb_puts(&b, "{\"cmd\":\"key\",\"keys\":[");
        for (int i = 0; i < ac; i++) {
            const char *k = av[i];
            for (int j = 0; KEYNAMES[j].name; j++) if (!strcasecmp(k, KEYNAMES[j].name)) { k = KEYNAMES[j].key; break; }
            if (i) sb_putc(&b, ',');
            json_put_string(&b, k, strlen(k));
        }
        sb_puts(&b, "]}");
        return send_cmd(port, &b, NULL);
    }
    if (!strcmp(cmd, "config")) {
        const char *theme = NULL, *font = NULL; long size = 0; int numbers = -1, wrap = -1;
        for (int i = 0; i < ac; i++) {
            if (!strcmp(av[i], "--theme") && i + 1 < ac) theme = av[++i];
            else if (!strcmp(av[i], "--font") && i + 1 < ac) font = av[++i];
            else if (!strcmp(av[i], "--size") && i + 1 < ac) size = atol(av[++i]);
            else if (!strcmp(av[i], "--numbers")) numbers = 1; else if (!strcmp(av[i], "--no-numbers")) numbers = 0;
            else if (!strcmp(av[i], "--wrap")) wrap = 1; else if (!strcmp(av[i], "--chop")) wrap = 0;
            else { fprintf(stderr, "winless config: unknown argument %s\n", av[i]); return 2; }
        }
        sb_init(&b); sb_puts(&b, "{\"cmd\":\"config\"");
        if (theme) jopt(&b, "theme", theme);
        if (font) jopt(&b, "font", font);
        if (size > 0) { jkey(&b, "size"); sb_printf(&b, "%ld", size); }
        if (numbers >= 0) { jkey(&b, "numbers"); sb_puts(&b, numbers ? "true" : "false"); }
        if (wrap >= 0) { jkey(&b, "wrap"); sb_puts(&b, wrap ? "true" : "false"); }
        sb_putc(&b, '}');
        return send_cmd(port, &b, NULL);
    }
    if (!strcmp(cmd, "flash")) {
        if (ac < 1) { fprintf(stderr, "usage: winless flash TEXT\n"); return 2; }
        sb_t t; sb_init(&t);
        for (int i = 0; i < ac; i++) { if (i) sb_putc(&t, ' '); sb_puts(&t, av[i]); }
        sb_init(&b); sb_puts(&b, "{\"cmd\":\"flash\""); jopt(&b, "text", t.s); sb_putc(&b, '}'); sb_free(&t);
        return send_cmd(port, &b, NULL);
    }
    if (!strcmp(cmd, "next") || !strcmp(cmd, "prev") || !strcmp(cmd, "clear") || !strcmp(cmd, "unmark") || !strcmp(cmd, "unhl") || !strcmp(cmd, "close"))
        return simple_cmd(port, cmd);

    fprintf(stderr, "winless: unknown command: %s\n", cmd);
    usage(stderr);
    return 2;
}
