/* remote.c -- winless over ssh.  Copyright (C) 2026 David Walther.  GPL-3.0-or-later.
 *
 * `winless ssh HOST` (on the desktop) opens one ssh connection that stays up (ssh's ControlMaster), forwards a private unix socket
 * from HOST back to a small relay process on the desktop (ssh -R /tmp/winless-X.sock:bell.sock), and gives you a shell on HOST with
 * $WINLESS_BELL naming that socket.  On HOST, `winless open FILE` starts its server next to the file as always; having no screen, it
 * "rings the bell" instead of launching a browser: the relay forwards the server's port to the desktop (ssh -O forward -L, through the
 * same connection) and opens the window there.  Everything else -- editing, writing, history, git, filters, the model -- runs on HOST.
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_LINE (512UL * 1024 * 1024)

static const char *ssh_bin(void) { const char *e = getenv("WINLESS_SSH"); return e && *e ? e : "ssh"; }

/* ------------------------------------------------------------------------------------------------ small helpers */
/* Run argv (no shell) with `in` on its standard input, collecting its standard output into `out` (may be NULL).  Returns the exit status,
 * or -1 if it could not run.  Its standard error is shown, unless `quiet`. */
static int run_capture(char *const argv[], const char *in, size_t inlen, sb_t *out, int quiet) {
    int pin[2], pout[2];
    if (pipe(pin) < 0) return -1;
    if (pipe(pout) < 0) { close(pin[0]); close(pin[1]); return -1; }
    pid_t pid = fork();
    if (pid < 0) { close(pin[0]); close(pin[1]); close(pout[0]); close(pout[1]); return -1; }
    if (pid == 0) {
        dup2(pin[0], 0); dup2(pout[1], 1);
        if (quiet) { int nul = open("/dev/null", O_WRONLY); if (nul >= 0) dup2(nul, 2); }
        close(pin[0]); close(pin[1]); close(pout[0]); close(pout[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pin[0]); close(pout[1]);
    int win = pin[1]; size_t off = 0;
    if (!inlen) { close(win); win = -1; } else fcntl(win, F_SETFL, fcntl(win, F_GETFL) | O_NONBLOCK);
    for (;;) {
        struct pollfd pf[2]; nfds_t n = 1;
        pf[0].fd = pout[0]; pf[0].events = POLLIN; pf[0].revents = 0;
        if (win >= 0) { pf[1].fd = win; pf[1].events = POLLOUT; pf[1].revents = 0; n = 2; }
        if (poll(pf, n, -1) < 0 && errno != EINTR) break;
        if (pf[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            char buf[65536]; ssize_t r = read(pout[0], buf, sizeof buf);
            if (r > 0) { if (out) sb_put(out, buf, (size_t)r); }
            else if (r == 0 || (errno != EINTR && errno != EAGAIN)) break;
        }
        if (win >= 0 && (pf[1].revents & (POLLOUT | POLLERR | POLLHUP))) {
            ssize_t w = write(win, in + off, inlen - off);
            if (w > 0) off += (size_t)w;
            else if (w < 0 && errno != EAGAIN && errno != EINTR) { close(win); win = -1; }
            if (win >= 0 && off >= inlen) { close(win); win = -1; }
        }
    }
    if (win >= 0) close(win);
    close(pout[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int run_plain(char *const argv[]) { return run_capture(argv, NULL, 0, NULL, 0); }       /* output to the screen */

static void msleep_(int ms) { struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }

static int sockaddr_for(struct sockaddr_un *a, const char *path) {
    memset(a, 0, sizeof *a); a->sun_family = AF_UNIX;
    if (strlen(path) >= sizeof a->sun_path) return -1;
    strcpy(a->sun_path, path);
    return 0;
}

/* ------------------------------------------------------------------------------------------------ the bell (remote side) */
int bell_available(void) {
    const char *p = getenv("WINLESS_BELL"); struct sockaddr_un a;
    if (!p || !*p || sockaddr_for(&a, p)) return 0;
    int s = socket(AF_UNIX, SOCK_STREAM, 0); if (s < 0) return 0;
    int ok = connect(s, (struct sockaddr *)&a, sizeof a) == 0;
    close(s);
    return ok;
}

int bell_call(const char *line, size_t n, char **reply) {
    const char *p = getenv("WINLESS_BELL"); struct sockaddr_un a;
    *reply = NULL;
    if (!p || !*p || sockaddr_for(&a, p)) return -1;
    int s = socket(AF_UNIX, SOCK_STREAM, 0); if (s < 0) return -1;
    struct timeval tv = { 60, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) { close(s); return -1; }
    size_t off = 0;
    while (off < n) {
        ssize_t w = send(s, line + off, n - off, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { close(s); return -1; }
        off += (size_t)w;
    }
    shutdown(s, SHUT_WR);
    sb_t in; sb_init(&in);
    char buf[65536]; ssize_t r;
    while ((r = recv(s, buf, sizeof buf, 0)) != 0) {
        if (r < 0) { if (errno == EINTR) continue; break; }
        sb_put(&in, buf, (size_t)r);
    }
    close(s);
    *reply = sb_detach(&in, NULL);
    return 0;
}

/* ------------------------------------------------------------------------------------------------ the relay (desktop side) */
typedef struct { const char *dir, *ctl, *dest; } relay_t;

static int port_is_free(int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0); if (s < 0) return 0;
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons((uint16_t)port);
    int ok = bind(s, (struct sockaddr *)&a, sizeof a) == 0;
    close(s);
    return ok;
}

static void reply_json(int fd, const char *s) { size_t n = strlen(s), off = 0; while (off < n) { ssize_t w = send(fd, s + off, n - off, MSG_NOSIGNAL); if (w <= 0) { if (w < 0 && errno == EINTR) continue; return; } off += (size_t)w; } }
static void reply_error(int fd, const char *msg) {
    sb_t b; sb_init(&b); sb_puts(&b, "{\"ok\":false,\"error\":"); json_put_string(&b, msg, strlen(msg)); sb_puts(&b, "}\n");
    reply_json(fd, b.s); sb_free(&b);
}

/* ssh -S ctl -O forward|cancel -L L:127.0.0.1:P DEST   (adds or removes a forward on the connection that is already up) */
static int ssh_forward(const relay_t *r, const char *op, int local, int remote) {
    char spec[64]; snprintf(spec, sizeof spec, "%d:127.0.0.1:%d", local, remote);
    char *argv[] = { (char *)ssh_bin(), "-S", (char *)r->ctl, "-O", (char *)op, "-L", spec, (char *)r->dest, NULL };
    return run_capture(argv, NULL, 0, NULL, 1);
}

/* window {port, token, windowed}: forward the remote server's port here and open the window on it. */
static void relay_window(const relay_t *r, int fd, const jv *v) {
    int remote = (int)json_getnum(v, "port", 0);
    const char *tok = json_gets(v, "token", NULL);
    if (remote < 1 || remote > 65535 || !tok || !*tok) { reply_error(fd, "bad window request"); return; }
    for (const char *p = tok; *p; p++) if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) { reply_error(fd, "bad token"); return; }
    int local = port_is_free(remote) ? remote : free_local_port();             /* the same port when we can: the URL then looks the same on both ends */
    if (!local) { reply_error(fd, "no free local port"); return; }
    if (ssh_forward(r, "forward", local, remote) != 0) { reply_error(fd, "ssh could not forward the port (is the ssh connection still up?)"); return; }
    char url[300]; snprintf(url, sizeof url, "http://localhost:%d/?t=%s", local, tok);
    if (!open_url_window(url, json_getbool(v, "windowed", 0))) {
        ssh_forward(r, "cancel", local, remote);
        reply_error(fd, "no browser found on the desktop (set WINLESS_BROWSER)"); return;
    }
    char ok[64]; snprintf(ok, sizeof ok, "{\"ok\":true,\"local\":%d}\n", local);
    reply_json(fd, ok);
    close(fd);
    /* stay as the forward's keeper: when the server on the other end is gone (its window closed), take the forward down */
    int misses = 0;
    for (;;) {
        msleep_(3000);
        if (server_alive(local)) misses = 0; else if (++misses >= 3) break;
    }
    ssh_forward(r, "cancel", local, remote);
}

static void relay_handle(const relay_t *r, int fd) {
    sb_t in; sb_init(&in);
    char buf[65536];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        sb_put(&in, buf, (size_t)n);
        if (in.len > MAX_LINE) { reply_error(fd, "request too large"); sb_free(&in); return; }
        if (memchr(in.s, '\n', in.len)) break;
    }
    jv *v = json_parse(in.s, in.len); sb_free(&in);
    const char *cmd = v ? json_gets(v, "cmd", NULL) : NULL;
    if (!cmd) reply_error(fd, "bad request");
    else if (!strcmp(cmd, "window")) { relay_window(r, fd, v); }
    else reply_error(fd, "unknown request");
    json_free(v);
}

int remote_relay_main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: winless relay DIR CTL DEST BELL (started by `winless ssh`)\n"); return 2; }
    relay_t r = { argv[0], argv[1], argv[2] };
    struct sockaddr_un a;
    if (sockaddr_for(&a, argv[3])) { fprintf(stderr, "winless relay: socket path too long\n"); return 1; }
    signal(SIGPIPE, SIG_IGN); signal(SIGCHLD, SIG_IGN);
    unlink(argv[3]);
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    mode_t old = umask(077);
    int bad = ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 16) < 0;
    umask(old);
    if (bad) { fprintf(stderr, "winless relay: cannot listen on %s: %s\n", argv[3], strerror(errno)); return 1; }
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; break; }
        pid_t pid = fork();
        if (pid == 0) { close(ls); signal(SIGCHLD, SIG_DFL); relay_handle(&r, fd); _exit(0); }     /* (the handler must see its own children's exit status) */
        close(fd);
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------------ winless ssh */
/* ssh options that take an argument (ssh(1)); the first word that is not an option or such an argument is the host */
static int opt_takes_arg(char c) { return c && strchr("BbcDEeFIiJLlmOoPpQRSWw", c) != NULL; }

/* a directory for the session's sockets: short (unix socket paths are limited to ~100 bytes) and private */
static int session_base(char *buf, size_t n) {
    const char *x = getenv("XDG_RUNTIME_DIR");
    if (x && *x && access(x, W_OK) == 0) snprintf(buf, n, "%s/winless", x);
    else { const char *t = getenv("TMPDIR"); snprintf(buf, n, "%s/winless-%ld", t && *t ? t : "/tmp", (long)getuid()); }
    mkdir(buf, 0700);
    struct stat st;
    return lstat(buf, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == getuid() && (st.st_mode & 077) == 0 ? 0 : -1;
}

/* ---- making sure the remote host has a winless that can ring the bell ----
 * Ask the host what it is (uname -sm) and whether a login shell already finds a winless that knows the bell.  If not, upload ours: a static
 * binary built for that platform (make builds one for the machine it runs on: remote/<Os-machine>/winless; others can be dropped into
 * ~/.local/share/winless/remote/ or $WINLESS_REMOTE_DIR) with viewer.html beside it, into ~/.cache/winless/remote/<hash>/ on the host.  An ssh
 * exec channel without a terminal is 8-bit clean, so the bytes go through `cat >` unchanged (no uuencode needed); the uploaded binary is then
 * run once on the host to prove it arrived and works there. */
static unsigned long long fnv64(unsigned long long h, const char *p, size_t n) { for (size_t i = 0; i < n; i++) { h ^= (unsigned char)p[i]; h *= 1099511628211ULL; } return h; }

static int find_platform_binary(const char *key, char *out, size_t n) {
    char dirs[3][1100]; int nd = 0;
    const char *e = getenv("WINLESS_REMOTE_DIR"); if (e && *e) snprintf(dirs[nd++], sizeof dirs[0], "%s", e);
    char data[1024]; winless_data_dir(data, sizeof data); snprintf(dirs[nd++], sizeof dirs[0], "%s/remote", data);
#ifdef PKGDATADIR
    snprintf(dirs[nd++], sizeof dirs[0], "%s/remote", PKGDATADIR);
#endif
    for (int i = 0; i < nd; i++) {
        int w = snprintf(out, n, "%s/%s/winless", dirs[i], key);
        if (w > 0 && (size_t)w < n && access(out, R_OK) == 0) return 1;          /* (a path that does not fit is not one we have) */
    }
    return 0;
}

/* On success sets *pathdir to the directory (as the host's shell will expand it) to put first on PATH, or "" if the host's own winless is used. */
static int prepare_remote(const char *ctl, const char *dest, char *pathdir, size_t pn) {
    pathdir[0] = 0;
    sb_t out; sb_init(&out);
    static const char probe[] =
        "printf 'WL-UNAME %s\\n' \"$(uname -sm)\"; "
        "\"${SHELL:-/bin/sh}\" -lc 'winless --features 2>/dev/null | while read f; do echo \"WL-FEATURE $f\"; done'";
    char *av[] = { (char *)ssh_bin(), "-S", (char *)ctl, (char *)dest, (char *)probe, NULL };
    if (run_capture(av, NULL, 0, &out, 1) < 0) { sb_free(&out); fprintf(stderr, "winless ssh: cannot inspect %s; the shell will work but not winless\n", dest); return -1; }
    char uname[128] = ""; int has_bell = 0;
    for (char *line = out.s, *nl; line && *line; line = nl ? nl + 1 : NULL) {
        nl = strchr(line, '\n'); if (nl) *nl = 0;
        if (!strncmp(line, "WL-UNAME ", 9)) snprintf(uname, sizeof uname, "%s", line + 9);
        else if (!strncmp(line, "WL-FEATURE ", 11) && !strncmp(line + 11, "bell", 4)) has_bell = 1;
        if (!nl) break;
    }
    sb_free(&out);
    if (has_bell) return 0;                                                 /* the host has its own, recent enough */
    if (!*uname) { fprintf(stderr, "winless ssh: %s did not say what it is; the shell will work but not winless\n", dest); return -1; }
    char key[128]; snprintf(key, sizeof key, "%s", uname);
    for (char *p = key; *p; p++) if (*p == ' ' || *p == '/') *p = '-';
    char bin[3300];
    if (!find_platform_binary(key, bin, sizeof bin)) {
        fprintf(stderr, "winless ssh: %s has no winless and I have none built for %s.\n"
                        "  Put a static build at ~/.local/share/winless/remote/%s/winless (or build it there: ./configure && make install).\n", dest, uname, key);
        return -1;
    }
    size_t bl, hl; char *bdata = read_file(bin, &bl), *hdata = read_file(viewer_html_path(), &hl);
    if (!bdata || !hdata) { fprintf(stderr, "winless ssh: cannot read %s or the viewer page\n", bin); free(bdata); free(hdata); return -1; }
    unsigned long long h = fnv64(fnv64(1469598103934665603ULL, bdata, bl), hdata, hl);
    char dir[160]; snprintf(dir, sizeof dir, "$HOME/.cache/winless/remote/%016llx", h);
    /* already there (an earlier session uploaded it) and it runs? */
    char cmd[600]; snprintf(cmd, sizeof cmd, "D=\"%s\"; [ -x \"$D/winless\" ] && \"$D/winless\" --features", dir);
    sb_init(&out);
    char *av1[] = { (char *)ssh_bin(), "-S", (char *)ctl, (char *)dest, cmd, NULL };
    run_capture(av1, NULL, 0, &out, 1);
    int have = out.len >= 4 && strstr(out.s, "bell") != NULL; sb_free(&out);
    if (!have) {
        fprintf(stderr, "winless ssh: uploading winless (%s, %.1f MB) to %s...\n", uname, (double)bl / 1048576.0, dest);
        snprintf(cmd, sizeof cmd, "D=\"%s\" && mkdir -p \"$D\" && cat > \"$D/winless.new.$$\" && chmod 755 \"$D/winless.new.$$\" && mv -f \"$D/winless.new.$$\" \"$D/winless\"", dir);
        char *av2[] = { (char *)ssh_bin(), "-S", (char *)ctl, (char *)dest, cmd, NULL };
        int ok = run_capture(av2, bdata, bl, NULL, 0) == 0;
        if (ok) {
            snprintf(cmd, sizeof cmd, "D=\"%s\" && cat > \"$D/viewer.html.new.$$\" && mv -f \"$D/viewer.html.new.$$\" \"$D/viewer.html\"", dir);
            char *av3[] = { (char *)ssh_bin(), "-S", (char *)ctl, (char *)dest, cmd, NULL };
            ok = run_capture(av3, hdata, hl, NULL, 0) == 0;
        }
        if (ok) {
            snprintf(cmd, sizeof cmd, "\"%s/winless\" --features", dir);
            sb_init(&out); char *av4[] = { (char *)ssh_bin(), "-S", (char *)ctl, (char *)dest, cmd, NULL };
            run_capture(av4, NULL, 0, &out, 1);
            ok = strstr(out.s, "bell") != NULL; sb_free(&out);
            if (!ok) fprintf(stderr, "winless ssh: the uploaded winless does not run on %s (%s)\n", dest, uname);
        } else fprintf(stderr, "winless ssh: the upload to %s failed\n", dest);
        if (!ok) { free(bdata); free(hdata); return -1; }
    }
    free(bdata); free(hdata);
    snprintf(pathdir, pn, "%s", dir);
    return 0;
}

static void remove_session(const char *dir) {
    char f[700];
    const char *names[] = { "bell.sock", "ctl", "relay.log", NULL };
    for (int i = 0; names[i]; i++) { snprintf(f, sizeof f, "%s/%s", dir, names[i]); unlink(f); }
    rmdir(dir);
}

int remote_ssh_main(int argc, char **argv) {
    const char *dest = NULL; int ai = 0;
    char *uopts[64]; int nuopts = 0;
    for (; ai < argc; ai++) {
        const char *a = argv[ai];
        if (a[0] != '-' || !a[1]) { dest = a; ai++; break; }
        if (nuopts + 2 >= 64) { fprintf(stderr, "winless ssh: too many options\n"); return 2; }
        uopts[nuopts++] = argv[ai];
        for (const char *p = a + 1; *p; p++) {
            if (strchr("MSNfOW", *p)) { fprintf(stderr, "winless ssh: -%c would interfere with the connection winless manages\n", *p); return 2; }
            if (opt_takes_arg(*p)) { if (!p[1]) { if (ai + 1 >= argc) { fprintf(stderr, "winless ssh: -%c needs an argument\n", *p); return 2; } uopts[nuopts++] = argv[++ai]; } break; }
        }
    }
    if (!dest) { fprintf(stderr, "usage: winless ssh [ssh-options] HOST [COMMAND]\n"); return 2; }
    char **cmd = argv + ai; int ncmd = argc - ai;

    signal(SIGPIPE, SIG_IGN);
    char base[512];
    if (session_base(base, sizeof base)) { fprintf(stderr, "winless ssh: %s is not a private directory of yours\n", base); return 1; }
    char tok[65];
    if (!token_make(tok, sizeof tok)) { fprintf(stderr, "winless ssh: cannot make a session id (/dev/urandom)\n"); return 1; }
    char dir[600], ctl[640], bell[640], rbell[80];
    snprintf(dir, sizeof dir, "%s/%.8s", base, tok);
    snprintf(ctl, sizeof ctl, "%s/ctl", dir); snprintf(bell, sizeof bell, "%s/bell.sock", dir);
    snprintf(rbell, sizeof rbell, "/tmp/winless-%.16s.sock", tok);                 /* on the remote host: random, and made 0600 by sshd */
    if (mkdir(dir, 0700) < 0) { fprintf(stderr, "winless ssh: cannot make %s: %s\n", dir, strerror(errno)); return 1; }

    pid_t relay = fork();                                                          /* the relay: it answers the bell */
    if (relay < 0) { remove_session(dir); return 1; }
    if (relay == 0) {
        setsid();
        char log[700]; snprintf(log, sizeof log, "%s/relay.log", dir);
        int nul = open("/dev/null", O_RDONLY), lg = open(log, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (nul >= 0) dup2(nul, 0);
        if (lg >= 0) { dup2(lg, 1); dup2(lg, 2); }
        char *ra[] = { dir, ctl, (char *)dest, bell };
        _exit(remote_relay_main(4, ra));
    }
    int ready = 0;
    for (int i = 0; i < 100 && !ready; i++) { struct stat st; if (stat(bell, &st) == 0) ready = 1; else msleep_(50); }
    if (!ready) { fprintf(stderr, "winless ssh: the relay did not start (see %s/relay.log)\n", dir); kill(-relay, SIGTERM); waitpid(relay, NULL, 0); remove_session(dir); return 1; }

    int rc = 1;
    /* 1. the connection: authenticates (on this terminal), then stays up in the background; the bell socket is forwarded back through it */
    {
        char *av[96]; int n = 0; char rspec[800];
        snprintf(rspec, sizeof rspec, "%s:%s", rbell, bell);
        av[n++] = (char *)ssh_bin();
        for (int i = 0; i < nuopts; i++) av[n++] = uopts[i];
        av[n++] = "-M"; av[n++] = "-S"; av[n++] = ctl; av[n++] = "-N"; av[n++] = "-f";
        av[n++] = "-o"; av[n++] = "ExitOnForwardFailure=yes"; av[n++] = "-o"; av[n++] = "StreamLocalBindUnlink=yes";
        av[n++] = "-R"; av[n++] = rspec; av[n++] = (char *)dest; av[n] = NULL;
        if (run_plain(av) != 0) { fprintf(stderr, "winless ssh: could not connect to %s\n", dest); goto done_relay; }
    }
    /* 2. a winless on the host that knows the bell (its own, or ours uploaded); if that cannot be arranged the shell still works */
    char pathdir[200];
    prepare_remote(ctl, dest, pathdir, sizeof pathdir);
    /* 3. the shell: through the same connection, with the bell's address (and our winless first on PATH, if uploaded) in its environment */
    {
        sb_t rc_cmd; sb_init(&rc_cmd);
        sb_printf(&rc_cmd, "export WINLESS_BELL='%s'; ", rbell);
        if (pathdir[0]) sb_printf(&rc_cmd, "export PATH=\"%s:$PATH\"; ", pathdir);
        if (ncmd) { for (int i = 0; i < ncmd; i++) { if (i) sb_putc(&rc_cmd, ' '); sb_puts(&rc_cmd, cmd[i]); } }
        else sb_puts(&rc_cmd, "exec \"${SHELL:-/bin/sh}\" -l");
        char *av[96]; int n = 0;
        av[n++] = (char *)ssh_bin(); av[n++] = "-S"; av[n++] = ctl;
        if (!ncmd) av[n++] = "-t";
        av[n++] = (char *)dest; av[n++] = rc_cmd.s; av[n] = NULL;
        rc = run_plain(av);
        sb_free(&rc_cmd);
    }
    {
        char *av[] = { (char *)ssh_bin(), "-S", ctl, "-O", "exit", (char *)dest, NULL };
        run_capture(av, NULL, 0, NULL, 1);
    }
done_relay:
    kill(-relay, SIGTERM);                                                         /* the relay and the forward keepers it started (its own process group) */
    { int st; waitpid(relay, &st, 0); }
    remove_session(dir);
    return rc < 0 ? 1 : rc;
}
