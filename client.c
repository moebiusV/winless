/* client.c -- the winless command line: talks to the server, starts it on demand, opens the window.
 * Copyright (C) 2026 David Walther.  GPL-3.0-or-later. */
#define _POSIX_C_SOURCE 200809L
#include "winless.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
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

static void ensure_server(int port) {
    if (server_alive(port)) return;
    const char *home = getenv("HOME"), *xdg = getenv("XDG_CACHE_HOME");
    char dir[1024], log[1100];
    if (xdg && *xdg) snprintf(dir, sizeof dir, "%s/winless", xdg); else snprintf(dir, sizeof dir, "%s/.cache/winless", home ? home : "/tmp");
    char tmp[1024]; snprintf(tmp, sizeof tmp, "%s", dir);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(dir, 0755);
    snprintf(log, sizeof log, "%s/server.log", dir);
    char pbuf[16]; snprintf(pbuf, sizeof pbuf, "%d", port);
    char *me = self_path();
    char *argv[] = { me, "--port", pbuf, "serve", NULL };
    if (!me || !spawn_detached(argv, 0, log)) { fprintf(stderr, "winless: cannot start the server\n"); exit(1); }
    for (int i = 0; i < 50; i++) { if (server_alive(port)) return; msleep(100); }
    fprintf(stderr, "winless: the server did not start (see %s)\n", log);
    exit(1);
}

/* POST a command (a JSON object body) to the server. */
static int send_cmd(int port, sb_t *json, char **resp) {
    ensure_server(port);
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
        char *argv[10]; int n = 0;
        argv[n++] = (char *)b; argv[n++] = app;
        argv[n++] = windowed ? "--window-size=1100,820" : "--start-fullscreen";
        argv[n++] = "--new-window"; argv[n++] = "--no-first-run"; argv[n++] = "--no-default-browser-check";
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
          "  open [FILE|-] [--name N] [--line N] [--search P] [--theme dark|light] [--font F] [--size PX]\n"
          "       [-N] [-S] [--windowed] [--no-window] [--wait]     show a file (or stdin)\n"
          "  goto LINE|N%|top|bottom      scroll [-p] N      search [-b] PATTERN   next  prev  clear\n"
          "  mark A[-B] [COLOR]  unmark   hl PATTERN [COLOR]  unhl   key KEY...   flash TEXT\n"
          "  config [--theme T] [--font F] [--size N] [--numbers|--no-numbers] [--wrap|--chop]\n"
          "  status [--json] [--text]     wait [--timeout S]     window [--windowed]   close\n"
          "  serve | start | stop         --version   --help\n"
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

static int cmd_open(int port, int argc, char **argv) {
    const char *file = NULL, *name = NULL, *search = NULL, *theme = NULL, *font = NULL;
    long line = 0, size = 0;
    int numbers = 0, chop = 0, wait = 0, nowin = 0, windowed = 0;
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
        else if (!strcmp(a, "--no-window")) nowin = 1;
        else if (!strcmp(a, "--windowed")) windowed = 1;
        else if (!strcmp(a, "-") && !file) file = a;
        else if (a[0] != '-' && !file) file = a;
        else { fprintf(stderr, "winless open: unknown argument %s\n", a); return 2; }
    }
    ensure_server(port);
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
    sb_puts(&b, "{\"cmd\":\"load\"");
    jopt(&b, "name", name);
    jkey(&b, "text"); json_put_string(&b, clean.s, clean.len); sb_free(&clean);
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
    if (send_cmd(port, &b, NULL)) return 1;
    if (!nowin) ensure_window(port, windowed);
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

static int cmd_status(int port, int argc, char **argv) {
    int json = 0, text = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) json = 1; else if (!strcmp(argv[i], "--text")) text = 1;
        else { fprintf(stderr, "winless status: unknown argument %s\n", argv[i]); return 2; }
    }
    ensure_server(port);
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
    ensure_server(port);
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

int client_main(int port, int argc, char **argv) {
    if (argc < 1) { usage(stderr); return 2; }
    const char *cmd = argv[0];
    int ac = argc - 1; char **av = argv + 1;
    sb_t b;

    if (!strcmp(cmd, "help")) { usage(stdout); return 0; }
    if (!strcmp(cmd, "serve")) return server_run(port);
    if (!strcmp(cmd, "start")) { ensure_server(port); printf("winless server on port %d\n", port); return 0; }
    if (!strcmp(cmd, "stop")) {
        if (server_alive(port)) { char *r = NULL; http_do(port, "POST", "/cmd", "{\"cmd\":\"stop\"}", 14, 5.0, &r, NULL); free(r); }
        return 0;
    }
    if (!strcmp(cmd, "open")) return cmd_open(port, ac, av);
    if (!strcmp(cmd, "status")) return cmd_status(port, ac, av);
    if (!strcmp(cmd, "wait")) return cmd_wait(port, ac, av);
    if (!strcmp(cmd, "window")) {
        int windowed = ac > 0 && !strcmp(av[0], "--windowed");
        ensure_server(port);
        return ensure_window(port, windowed) ? 0 : 1;
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
