/* test_remote.c -- `winless ssh`, with a fake ssh and a fake browser.  Copyright (C) 2026 David Walther.  GPL-3.0-or-later.
 *
 * The fake ssh logs its arguments; for -M it makes the remote socket path a symlink to the local one (as the real -R would make it
 * reachable), for -O it succeeds, and otherwise it runs the last argument as the "remote" command with sh.  The fake browser logs its URL. */
#define _POSIX_C_SOURCE 200809L
#include "testlib.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

static char g_log[700], g_link[700], g_blog[700];

static char *make_script(const char *name, const char *body) {
    char *p = tmp_write(name, body, strlen(body));
    chmod(p, 0755);
    return p;
}

static char *slurp(const char *path) {
    size_t n; char *d = read_file(path, &n);
    return d ? d : xstrdup("");
}

static void setup_fakes(void) {
    snprintf(g_log, sizeof g_log, "%s/ssh.log", tmp_dir());
    snprintf(g_link, sizeof g_link, "%s/ssh.link", tmp_dir());
    snprintf(g_blog, sizeof g_blog, "%s/browser.log", tmp_dir());
    remove(g_log); remove(g_link); remove(g_blog);
    setenv("FAKE_LOG", g_log, 1); setenv("FAKE_LINK", g_link, 1); setenv("FAKE_BROWSER_LOG", g_blog, 1);
    unsetenv("FAKE_FAIL"); unsetenv("FAKE_FWD_FAIL"); unsetenv("WINLESS_BELL");
    setenv("WINLESS_SSH", make_script("fakessh.sh",
        "#!/bin/sh\n"
        "echo \"$*\" >> \"$FAKE_LOG\"\n"
        "case \" $* \" in\n"
        "*\" -M \"*)\n"
        "  [ -n \"$FAKE_FAIL\" ] && exit 255\n"
        "  while [ $# -gt 0 ]; do if [ \"$1\" = -R ]; then spec=$2; break; fi; shift; done\n"
        "  ln -s \"${spec#*:}\" \"${spec%%:*}\"\n"
        "  echo \"${spec%%:*}\" > \"$FAKE_LINK\"\n"
        "  exit 0;;\n"
        "*\" -O exit \"*) rm -f \"$(cat \"$FAKE_LINK\")\"; exit 0;;\n"
        "*\" -O forward \"*) [ -n \"$FAKE_FWD_FAIL\" ] && exit 255; exit 0;;\n"
        "*\" -O \"*) exit 0;;\n"
        "esac\n"
        "for last; do :; done\n"
        "PATH=\"${FAKE_REMOTE_PATH:-$PATH}\"; export PATH\n"          /* what the "remote" host has installed */
        "SHELL=\"$FAKE_SHELL\"; export SHELL\n"                        /* a login shell that does not read this machine's profile */
        "exec sh -c \"$last\"\n"), 1);
    setenv("FAKE_SHELL", make_script("fakeshell.sh", "#!/bin/sh\n[ \"$1\" = -lc ] && shift\nexec sh -c \"$1\"\n"), 1);
    char rbin[700]; snprintf(rbin, sizeof rbin, "%s/rbin", tmp_dir()); mkdir(rbin, 0755);
    char rpath[1000]; snprintf(rpath, sizeof rpath, "%s/winless", rbin);
    FILE *mock = fopen(rpath, "w");                                    /* the host already has a winless that knows the bell */
    if (mock) { fputs("#!/bin/sh\nif [ \"$1\" = --features ]; then echo bell; fi\n", mock); fclose(mock); chmod(rpath, 0755); }
    snprintf(rpath, sizeof rpath, "%s:/usr/bin:/bin", rbin);
    setenv("FAKE_REMOTE_PATH", rpath, 1);
    setenv("WINLESS_BROWSER", make_script("fakebrowser.sh", "#!/bin/sh\necho \"$1\" >> \"$FAKE_BROWSER_LOG\"\n"), 1);
}

/* run `winless ssh ARGS...` in the background */
static pid_t spawn_ssh(char *const args[]) {
    pid_t pid = fork();
    if (pid < 0) FAIL("fork failed");
    if (pid == 0) {
        size_t n = 0; while (args[n]) n++;
        char **argv = xmalloc((n + 3) * sizeof *argv);
        argv[0] = (char *)g_bin; argv[1] = "ssh";
        for (size_t i = 0; i < n; i++) argv[2 + i] = args[i];
        argv[2 + n] = NULL;
        execv(g_bin, argv);
        _exit(127);
    }
    return pid;
}

static int wait_exit(pid_t pid, double timeout) {
    int st = 0;
    for (int i = 0; i < (int)(timeout * 50); i++) {
        if (waitpid(pid, &st, WNOHANG) == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
        msleep(20);
    }
    kill(pid, SIGKILL); waitpid(pid, &st, 0);
    FAIL("`winless ssh` did not finish within %.0f s", timeout);
}

static int log_has(const char *needle) { char *l = slurp(g_log); int r = strstr(l, needle) != NULL; free(l); return r; }

static void t_a_remote_open_rings_the_bell_and_the_desktop_opens_the_window(void) {
    setup_fakes();
    char *f = tmp_write("remote-notes.txt", "alpha\nbeta\n", 11);
    pid_t pid = spawn_ssh((char *const[]){ "fakehost", (char *)g_bin, "open", f, NULL });
    /* the browser is asked for a URL once the port is forwarded */
    char *blog = NULL;
    for (int i = 0; i < 300; i++) { free(blog); blog = slurp(g_blog); if (strchr(blog, '\n')) break; msleep(50); }
    CHECK_CONTAINS(blog, "--app=http://localhost:");
    CHECK_CONTAINS(blog, "/?t=");
    char *log = slurp(g_log);
    char *fw = strstr(log, "-O forward -L ");
    if (!fw) FAIL("no forward was asked for; ssh log:\n%s", log);
    int local = 0, remote = 0;
    if (sscanf(fw, "-O forward -L %d:127.0.0.1:%d", &local, &remote) != 2) FAIL("cannot read the forward in [%.200s]", fw);
    char want[100]; snprintf(want, sizeof want, "http://localhost:%d/?t=", local);
    CHECK_CONTAINS(blog, want);
    g_port = remote;                                           /* the server the file was opened on */
    const char *tok = test_token();
    CHECK_INT(strlen(tok), 64);
    snprintf(want, sizeof want, "/?t=%s", tok);
    CHECK_CONTAINS(blog, want);                                /* the window is opened with that server's own secret */
    sse_t *page = sse_open();                                  /* the "window" connects (through the forward, in real life) */
    CHECK_INT(wait_exit(pid, 30), 0);                          /* the remote `winless open` saw the window and finished; so did the session */
    free(log); log = slurp(g_log);                             /* (now with the end of the session in it) */
    CHECK_CONTAINS(log, "-M -S ");
    CHECK_CONTAINS(log, "-R /tmp/winless-");
    CHECK_CONTAINS(log, "-O exit");
    CHECK(log_has("-S "));
    sse_close(page);
    free(blog); free(log);
    g_port = 0;
}

static void t_ssh_options_pass_to_the_connection_and_the_command_runs_there(void) {
    setup_fakes();
    char *out = make_script("echo-bell.sh", "#!/bin/sh\necho \"bell=$WINLESS_BELL\" > \"$FAKE_LOG.env\"\n");
    pid_t pid = spawn_ssh((char *const[]){ "-p", "2222", "-o", "Compression=no", "-v", "me@fakehost", out, NULL });
    CHECK_INT(wait_exit(pid, 30), 0);
    char *log = slurp(g_log);
    CHECK_CONTAINS(log, "-p 2222 -o Compression=no -v -M ");   /* the user's options go to the connection ... */
    CHECK_CONTAINS(log, "me@fakehost");
    char p[760]; snprintf(p, sizeof p, "%s.env", g_log);
    char *env = slurp(p);
    CHECK_CONTAINS(env, "bell=/tmp/winless-");                 /* ... and the command sees the bell's address */
    free(env); free(log);
}

static void t_a_failed_connection_cleans_up(void) {
    setup_fakes();
    setenv("FAKE_FAIL", "1", 1);
    pid_t pid = spawn_ssh((char *const[]){ "fakehost", NULL });
    CHECK_INT(wait_exit(pid, 30), 1);
    CHECK(!log_has("-O exit"));
    unsetenv("FAKE_FAIL");
}

static void t_options_that_would_break_the_connection_are_refused(void) {
    setup_fakes();
    pid_t pid = spawn_ssh((char *const[]){ "-S", "/tmp/x", "fakehost", NULL });
    CHECK_INT(wait_exit(pid, 10), 2);
    pid = spawn_ssh((char *const[]){ NULL });
    CHECK_INT(wait_exit(pid, 10), 2);
    pid = spawn_ssh((char *const[]){ "-fN", "fakehost", NULL });
    CHECK_INT(wait_exit(pid, 10), 2);
    CHECK_INT(slurp(g_log)[0], 0);                             /* ssh was never run */
}

static void t_a_forward_that_ssh_refuses_is_reported(void) {
    setup_fakes();
    setenv("FAKE_FWD_FAIL", "1", 1);
    char *f = tmp_write("remote-notes2.txt", "x\n", 2);
    pid_t pid = spawn_ssh((char *const[]){ "fakehost", (char *)g_bin, "open", f, NULL });
    int code = wait_exit(pid, 40);
    CHECK_INT(code, 0);                                        /* the shell session itself ended normally: `open` only warned */
    char *blog = slurp(g_blog);
    CHECK_INT(blog[0], 0);                                     /* and no window was opened */
    free(blog);
    unsetenv("FAKE_FWD_FAIL");
}

static int lines_with(const char *text, const char *needle) {
    int n = 0;
    for (const char *p = text; *p;) { const char *nl = strchr(p, '\n'); size_t len = nl ? (size_t)(nl - p) : strlen(p); char *l = xstrndup(p, len); if (strstr(l, needle)) n++; free(l); p += len + (nl ? 1 : 0); }
    return n;
}

static void copy_file(const char *from, const char *to) {
    size_t n; char *d = read_file(from, &n);
    FILE *f = d ? fopen(to, "wb") : NULL;
    if (!f) FAIL("cannot copy %s to %s", from, to);
    fwrite(d, 1, n, f); fclose(f); chmod(to, 0755); free(d);
}

/* The host already has a winless that knows the bell: use it, upload nothing. */
static void t_a_host_with_its_own_winless_is_left_alone(void) {
    setup_fakes();
    char *probe = make_script("which.sh", "#!/bin/sh\ncommand -v winless > \"$FAKE_LOG.which\"\n");
    pid_t pid = spawn_ssh((char *const[]){ "fakehost", probe, NULL });
    CHECK_INT(wait_exit(pid, 30), 0);
    char *log = slurp(g_log);
    CHECK_INT(lines_with(log, "mkdir -p"), 0);                 /* nothing was uploaded */
    char p[760]; snprintf(p, sizeof p, "%s.which", g_log);
    char *which = slurp(p);
    CHECK_CONTAINS(which, "/rbin/winless");                    /* the shell still sees the host's own */
    free(which); free(log);
}

/* A host with no winless gets ours: uploaded once (byte for byte), put first on the shell's PATH, and reused by later sessions. */
static void t_a_host_without_winless_gets_ours_uploaded_once(void) {
    setup_fakes();
    setenv("FAKE_REMOTE_PATH", "/usr/bin:/bin", 1);            /* nothing installed there */
    struct utsname u; if (uname(&u)) FAIL("uname failed");
    char dist[400], plat[600], bin[700];
    snprintf(dist, sizeof dist, "%s/rdist", tmp_dir()); mkdir(dist, 0755);
    snprintf(plat, sizeof plat, "%s/%s-%s", dist, u.sysname, u.machine); mkdir(plat, 0755);
    snprintf(bin, sizeof bin, "%s/winless", plat); copy_file(g_bin, bin);
    setenv("WINLESS_REMOTE_DIR", dist, 1);
    char *probe = make_script("which2.sh", "#!/bin/sh\ncommand -v winless > \"$FAKE_LOG.which\"\nwinless --features > \"$FAKE_LOG.feat\"\n");
    for (int round = 0; round < 2; round++) {
        pid_t pid = spawn_ssh((char *const[]){ "fakehost", probe, NULL });
        CHECK_INT(wait_exit(pid, 60), 0);
        char p[760]; snprintf(p, sizeof p, "%s.which", g_log);
        char *which = slurp(p);
        { size_t wl = strlen(which); while (wl && (which[wl - 1] == '\n' || which[wl - 1] == '\r')) which[--wl] = 0; }
        CHECK_CONTAINS(which, "/.cache/winless/remote/");     /* the uploaded one comes first on PATH ... */
        snprintf(p, sizeof p, "%s.feat", g_log);
        char *feat = slurp(p);
        CHECK_CONTAINS(feat, "bell");                          /* ... and it runs */
        char dir[900]; snprintf(dir, sizeof dir, "%s", which); *strrchr(dir, '/') = 0;
        char html[1000]; snprintf(html, sizeof html, "%s/viewer.html", dir);
        CHECK_INT(access(html, R_OK), 0);                      /* the page is beside it */
        struct stat a, b; CHECK(stat(which, &a) == 0 && stat(bin, &b) == 0);
        CHECK_INT(a.st_size, b.st_size);                       /* byte for byte */
        free(which); free(feat);
    }
    char *log = slurp(g_log);
    CHECK_INT(lines_with(log, "mkdir -p"), 1);                 /* the second session found it already there */
    free(log);
    unsetenv("WINLESS_REMOTE_DIR");
}

/* Outside a session (or with a bell nobody answers) `open` behaves as it always did: it launches the browser itself -- now on a URL carrying the secret. */
static void t_a_dead_bell_means_a_local_window_with_the_secret(void) {
    setup_fakes();
    setenv("WINLESS_BELL", "/nonexistent/bell.sock", 1);
    char *f = tmp_write("local-notes.txt", "x\n", 2);
    char outf[700]; snprintf(outf, sizeof outf, "%s/open.out", tmp_dir()); remove(outf);
    pid_t pid = fork();
    if (pid < 0) FAIL("fork failed");
    if (pid == 0) {
        int fd = open(outf, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd >= 0) dup2(fd, 1);
        execl(g_bin, g_bin, "open", f, (char *)NULL);
        _exit(127);
    }
    char *blog = NULL;                                          /* (`open` reports its port only after the window connects, so read it from the URL) */
    for (int i = 0; i < 200; i++) { free(blog); blog = slurp(g_blog); if (strchr(blog, '\n')) break; msleep(50); }
    const char *u = strstr(blog, "--app=http://localhost:");
    CHECK(u != NULL);
    int port = atoi(u + strlen("--app=http://localhost:"));
    CHECK(port > 0);
    g_port = port;
    char want[160]; snprintf(want, sizeof want, "--app=http://localhost:%d/?t=%s", port, test_token());
    CHECK_CONTAINS(blog, want);
    sse_t *page = sse_open();
    CHECK_INT(wait_exit(pid, 30), 0);
    sse_close(page);
    free(blog);
    unsetenv("WINLESS_BELL");
    g_port = 0;
}

int main(void) {
    testlib_init();
    static const testcase tests[] = {
        { "a_remote_open_rings_the_bell_and_the_desktop_opens_the_window", t_a_remote_open_rings_the_bell_and_the_desktop_opens_the_window, 0 },
        { "ssh_options_pass_to_the_connection_and_the_command_runs_there", t_ssh_options_pass_to_the_connection_and_the_command_runs_there, 0 },
        { "a_failed_connection_cleans_up", t_a_failed_connection_cleans_up, 0 },
        { "options_that_would_break_the_connection_are_refused", t_options_that_would_break_the_connection_are_refused, 0 },
        { "a_forward_that_ssh_refuses_is_reported", t_a_forward_that_ssh_refuses_is_reported, 0 },
        { "a_host_with_its_own_winless_is_left_alone", t_a_host_with_its_own_winless_is_left_alone, 0 },
        { "a_host_without_winless_gets_ours_uploaded_once", t_a_host_without_winless_gets_ours_uploaded_once, 0 },
        { "a_dead_bell_means_a_local_window_with_the_secret", t_a_dead_bell_means_a_local_window_with_the_secret, 0 },
    };
    return run_tests(tests, sizeof tests / sizeof *tests) ? 1 : 0;
}
