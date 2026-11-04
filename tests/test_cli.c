/* test_cli.c -- black-box tests of the winless command line against a real server process.
 * Copyright (C) 2026 David Walther.  GPL-3.0-or-later. */
#define _POSIX_C_SOURCE 200809L
#include "testlib.h"
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static jv *post_json(const char *path, const char *body) {
    char *r = post_ok(path, body);
    jv *j = json_parse(r, strlen(r));
    if (!j) FAIL("POST %s returned invalid JSON: %.200s", path, r);
    free(r);
    return j;
}

static void expect_ok(run_t r) {
    if (r.code != 0) FAIL("command failed with exit code %d: %.300s", r.code, r.out ? r.out : "");
    free(r.out);
}

static void t_open_status_goto_key(void) {
    char *f = tmp_write("t.txt", "alpha\nbeta\n", 11);
    expect_ok(CLI("open", f, "--no-window", "--theme", "light", "-N", "--search", "beta"));
    jv *c = get_json("/content");
    CHECK_STR(json_gets(c, "name", NULL), "t.txt");
    CHECK_STR(json_gets(c, "text", NULL), "alpha\nbeta\n");
    CHECK_STR(json_gets(c, "search", NULL), "beta");
    CHECK_STR(json_gets(json_get(c, "settings"), "theme", NULL), "light");
    CHECK_INT(json_getbool(json_get(c, "settings"), "numbers", 0), 1);
    json_free(c);

    sse_t *page = sse_open();
    wait_clients(1);
    expect_ok(CLI("goto", "50%"));
    expect_ok(CLI("key", "space", "G", "esc"));
    expect_ok(CLI("scroll", "-p", "-1"));
    expect_ok(CLI("goto", "bottom"));
    CHECK_INT(sse_wait(page, 4, 5), 4);
    CHECK_STR(sse_event(page, 0), "{\"cmd\":\"goto\",\"percent\":50}");
    CHECK_STR(sse_event(page, 1), "{\"cmd\":\"key\",\"keys\":[\" \",\"G\",\"Escape\"]}");
    CHECK_STR(sse_event(page, 2), "{\"cmd\":\"scroll\",\"n\":-1,\"page\":true}");
    CHECK_STR(sse_event(page, 3), "{\"cmd\":\"goto\",\"where\":\"bottom\"}");

    json_free(post_json("/event", "{\"type\":\"state\",\"name\":\"t.txt\",\"top\":1,\"bottom\":2,\"total\":2,\"atEnd\":true,"
                                  "\"visible\":[\"alpha\",\"beta\"],\"width\":100,\"height\":50,\"search\":\"beta\","
                                  "\"matchLines\":1,\"curLine\":2,\"numbers\":true,\"wrap\":true,\"theme\":\"light\"}"));
    run_t st = CLI("status", "--text");
    CHECK_INT(st.code, 0);
    CHECK_CONTAINS(st.out, "t.txt: lines 1-2 of 2 (100%)  [END]");
    CHECK_CONTAINS(st.out, "search /beta: 1 line(s); current line 2");
    CHECK_CONTAINS(st.out, "window 100x50px  numbers=true wrap=true theme=light");
    CHECK_CONTAINS(st.out, "--- visible ---\nalpha\nbeta\n");
    free(st.out);
    run_t sj = CLI("status", "--json");
    jv *j = json_parse(sj.out, sj.outlen);
    CHECK(j != NULL);
    CHECK_INT(json_getnum(json_get(j, "view"), "total", 0), 2);
    json_free(j); free(sj.out);
    sse_close(page);
}

static void t_open_from_stdin_and_cleans_invalid_utf8(void) {
    const char in[] = "ok \xFF\xFE bad \xE2\x82 cut\nline2 \xE2\x82\xAC euro\n";
    run_t r = CLI_IN(in, sizeof in - 1, "open", "-", "--no-window");
    CHECK_INT(r.code, 0); free(r.out);
    jv *c = get_json("/content");
    CHECK_STR(json_gets(c, "name", NULL), "(stdin)");
    CHECK_STR(json_gets(c, "text", NULL), "ok \xEF\xBF\xBD\xEF\xBF\xBD bad \xEF\xBF\xBD\xEF\xBF\xBD cut\nline2 \xE2\x82\xAC euro\n");
    json_free(c);
}

static void t_open_options_and_line(void) {
    char *f = tmp_write("o.txt", "1\n2\n3\n", 6);
    expect_ok(CLI("open", f, "--no-window", "--name", "custom", "--line", "3", "--font", "Mono", "--size", "21", "-S"));
    jv *c = get_json("/content");
    CHECK_STR(json_gets(c, "name", NULL), "custom");
    CHECK_INT(json_getnum(c, "top", 0), 3);
    const jv *s = json_get(c, "settings");
    CHECK_STR(json_gets(s, "font", NULL), "Mono");
    CHECK_INT(json_getnum(s, "size", 0), 21);
    CHECK_INT(json_getbool(s, "wrap", 1), 0);                  /* -S = chop long lines */
    json_free(c);
}

static void t_mark_hl_search_config_events(void) {
    sse_t *page = sse_open();
    wait_clients(1);
    expect_ok(CLI("mark", "10-20", "green"));
    expect_ok(CLI("mark", "7"));
    expect_ok(CLI("hl", "fox", "blue"));
    expect_ok(CLI("search", "-b", "needle"));
    expect_ok(CLI("config", "--size", "22", "--no-numbers", "--chop"));
    CHECK_INT(sse_wait(page, 5, 5), 5);
    CHECK_STR(sse_event(page, 0), "{\"cmd\":\"mark\",\"a\":10,\"b\":20,\"color\":\"green\"}");
    CHECK_STR(sse_event(page, 1), "{\"cmd\":\"mark\",\"a\":7,\"b\":7,\"color\":null}");
    CHECK_STR(sse_event(page, 2), "{\"cmd\":\"hl\",\"pattern\":\"fox\",\"color\":\"blue\"}");
    CHECK_STR(sse_event(page, 3), "{\"cmd\":\"search\",\"pattern\":\"needle\",\"backward\":true}");
    CHECK_STR(sse_event(page, 4), "{\"cmd\":\"config\",\"size\":22,\"numbers\":false,\"wrap\":false}");
    jv *c = get_json("/content");
    const jv *s = json_get(c, "settings");
    CHECK_INT(json_getnum(s, "size", 0), 22); CHECK_INT(json_getbool(s, "numbers", 1), 0); CHECK_INT(json_getbool(s, "wrap", 1), 0);
    json_free(c);
    sse_close(page);
}

static void t_simple_commands_and_variants(void) {
    sse_t *page = sse_open();
    wait_clients(1);
    expect_ok(CLI("goto", "120"));
    expect_ok(CLI("goto", "TOP"));
    expect_ok(CLI("scroll", "5"));
    expect_ok(CLI("flash", "hello", "there"));
    expect_ok(CLI("next")); expect_ok(CLI("prev")); expect_ok(CLI("clear")); expect_ok(CLI("unmark")); expect_ok(CLI("unhl")); expect_ok(CLI("close"));
    CHECK_INT(sse_wait(page, 10, 5), 10);
    CHECK_STR(sse_event(page, 0), "{\"cmd\":\"goto\",\"line\":120}");
    CHECK_STR(sse_event(page, 1), "{\"cmd\":\"goto\",\"where\":\"top\"}");
    CHECK_STR(sse_event(page, 2), "{\"cmd\":\"scroll\",\"n\":5,\"page\":false}");
    CHECK_STR(sse_event(page, 3), "{\"cmd\":\"flash\",\"text\":\"hello there\"}");
    CHECK_STR(sse_event(page, 4), "{\"cmd\":\"next\"}");
    CHECK_STR(sse_event(page, 5), "{\"cmd\":\"prev\"}");
    CHECK_STR(sse_event(page, 6), "{\"cmd\":\"clear\"}");
    CHECK_STR(sse_event(page, 7), "{\"cmd\":\"unmark\"}");
    CHECK_STR(sse_event(page, 8), "{\"cmd\":\"unhl\"}");
    CHECK_STR(sse_event(page, 9), "{\"cmd\":\"close\"}");
    sse_close(page);
}

static void t_status_without_a_window(void) {
    run_t r = CLI("status");
    CHECK_INT(r.code, 0);
    CHECK_CONTAINS(r.out, "no window connected");
    free(r.out);
}

static void t_rejects_bad_arguments(void) {
    CHECK_INT(CLI("goto", "nonsense").code, 2);
    CHECK_INT(CLI("goto").code, 2);
    CHECK_INT(CLI("bogus").code, 2);
    CHECK_INT(CLI("open", "--nope").code, 2);
    CHECK_INT(CLI("scroll").code, 2);
    CHECK_INT(CLI("search").code, 2);
    CHECK_INT(CLI("mark").code, 2);
    CHECK_INT(CLI("key").code, 2);
    CHECK_INT(cli_run(NULL, 0, (char *const[]){ NULL }).code, 2);          /* no command at all */
    CHECK_INT(CLI("open", "/definitely/not/a/file", "--no-window").code, 1);
}

static void t_wait_returns_when_quit_arrives(void) {
    int fd;
    pid_t pid = cli_spawn((char *const[]){ "wait", NULL }, &fd);
    msleep(500);
    int st;
    CHECK(waitpid(pid, &st, WNOHANG) == 0);                    /* still blocked */
    json_free(post_json("/event", "{\"type\":\"quit\"}"));
    run_t r = cli_finish(pid, fd, 10);
    CHECK_INT(r.code, 0);
    CHECK_STR(r.out, "quit\n");
    free(r.out);
}

static void t_wait_times_out_with_status_1(void) {
    run_t r = CLI("wait", "--timeout", "0.4");
    CHECK_INT(r.code, 1);
    CHECK_STR(r.out, "timeout\n");
    free(r.out);
}

static void t_open_wait_blocks_until_the_window_closes(void) {
    char *f = tmp_write("w.txt", "x\n", 2);
    int fd;
    pid_t pid = cli_spawn((char *const[]){ "open", f, "--no-window", "--wait", NULL }, &fd);
    msleep(600);
    int st;
    CHECK(waitpid(pid, &st, WNOHANG) == 0);
    json_free(post_json("/event", "{\"type\":\"closed\"}"));
    run_t r = cli_finish(pid, fd, 10);
    CHECK_INT(r.code, 0);
    free(r.out);
}

/* No fixture server: the CLI must start its own, and stop it on request. */
static void t_cli_starts_and_stops_its_own_server(void) {
    g_port = free_port();
    expect_ok(CLI("start"));
    char *p = get_ok("/ping");
    CHECK_CONTAINS(p, "\"ok\":true"); free(p);
    run_t st = CLI("status");
    CHECK_CONTAINS(st.out, "no window connected"); free(st.out);
    expect_ok(CLI("stop"));
    int gone = 0;
    for (int i = 0; i < 100 && !gone; i++) { resp_t r = http("GET", "/ping", NULL, 0); gone = r.status == 0; free(r.body); if (!gone) msleep(50); }
    CHECK(gone);
    /* its log is where the documentation says, and it holds no sanitizer report */
    char path[700]; snprintf(path, sizeof path, "%s/home/.cache/winless/server.log", tmp_dir());
    size_t n; char *log = read_file(path, &n);
    CHECK(log != NULL);
    CHECK(strstr(log, "AddressSanitizer") == NULL && strstr(log, "runtime error") == NULL);
    CHECK_CONTAINS(log, "serving on http://127.0.0.1:");
    free(log);
}

/* Any command auto-starts the server too. */
static void t_any_command_autostarts_the_server(void) {
    g_port = free_port();
    expect_ok(CLI("goto", "5"));
    char *p = get_ok("/ping");
    CHECK_CONTAINS(p, "\"ok\":true"); free(p);
    expect_ok(CLI("stop"));
    for (int i = 0; i < 100; i++) { resp_t r = http("GET", "/ping", NULL, 0); int gone = r.status == 0; free(r.body); if (gone) return; msleep(50); }
    FAIL("server still answering after stop");
}

static void t_version_and_help(void) {
    run_t v = cli_run(NULL, 0, (char *const[]){ "--version", NULL });
    CHECK_INT(v.code, 0);
    CHECK_STR(v.out, "winless " PACKAGE_VERSION "\n");
    free(v.out);
    run_t h = cli_run(NULL, 0, (char *const[]){ "--help", NULL });
    CHECK_INT(h.code, 0);
    CHECK_CONTAINS(h.out, "usage: winless");
    free(h.out);
}

static char *content_text(void) {            /* the server's current text (caller frees) */
    jv *c = get_json("/content");
    char *t = xstrdup(json_gets(c, "text", NULL)); json_free(c);
    return t;
}
static int content_dirty(void) { jv *c = get_json("/content"); int d = json_getbool(c, "dirty", -1); json_free(c); return d; }

static void t_edit_undo_write(void) {
    char *f = tmp_write("edit.txt", "alpha\nbeta\ngamma\n", 17);
    expect_ok(CLI("open", f, "--no-window"));
    jv *c = get_json("/content");
    CHECK_STR(json_gets(c, "path", NULL), f);                                  /* the server learns the file's path */
    CHECK_INT(json_getbool(c, "dirty", -1), 0);
    json_free(c);

    sse_t *page = sse_open(); wait_clients(1);
    expect_ok(CLI("edit", "2", "1", "2", "5", "BETA"));
    char *t = content_text(); CHECK_STR(t, "alpha\nBETA\ngamma\n"); free(t);
    CHECK_INT(content_dirty(), 1);
    CHECK_INT(sse_wait(page, 1, 5), 1);
    CHECK_STR(sse_event(page, 0), "{\"cmd\":\"reload\"}");                      /* pages re-read the text, keeping their place */
    size_t dl; char *disk = read_file(f, &dl); CHECK_STR(disk, "alpha\nbeta\ngamma\n"); free(disk);   /* not written yet */

    expect_ok(CLI("edit", "3", "1", "3", "4", "\xC3\xA9t\xC3\xA9"));            /* multibyte replacement: "gam" -> "été" */
    t = content_text(); CHECK_STR(t, "alpha\nBETA\n\xC3\xA9t\xC3\xA9ma\n"); free(t);
    expect_ok(CLI("edit", "3", "4", "3", "4", "!"));                           /* an insertion: columns count code points */
    t = content_text(); CHECK_STR(t, "alpha\nBETA\n\xC3\xA9t\xC3\xA9!ma\n"); free(t);

    expect_ok(CLI("write"));
    disk = read_file(f, &dl); CHECK_STR(disk, "alpha\nBETA\n\xC3\xA9t\xC3\xA9!ma\n"); free(disk);
    CHECK_INT(content_dirty(), 0);

    expect_ok(CLI("undo")); expect_ok(CLI("undo"));
    t = content_text(); CHECK_STR(t, "alpha\nBETA\ngamma\n"); free(t);
    CHECK_INT(content_dirty(), 1);                                             /* the file on disk still has the later text */
    expect_ok(CLI("undo"));
    t = content_text(); CHECK_STR(t, "alpha\nbeta\ngamma\n"); free(t);
    run_t u = CLI("undo");                                                     /* nothing left */
    CHECK_INT(u.code, 1); free(u.out);
    jv *e = post_json("/cmd", "{\"cmd\":\"undo\"}"); CHECK_STR(json_gets(e, "error", NULL), "nothing to undo"); json_free(e);
    sse_close(page);
}

static void t_edit_rejects_bad_ranges_and_changed_files(void) {
    char *f = tmp_write("guard.txt", "one\ntwo\n", 8);
    expect_ok(CLI("open", f, "--no-window"));
    run_t r = CLI("edit", "9", "1", "9", "1", "x");
    CHECK_INT(r.code, 1); free(r.out);
    jv *e = post_json("/cmd", "{\"cmd\":\"edit\",\"r1\":9,\"c1\":1,\"r2\":9,\"c2\":1,\"text\":\"x\"}");
    CHECK_STR(json_gets(e, "error", NULL), "range out of bounds"); json_free(e);
    r = CLI("edit", "2", "1", "1", "1", "x");                                  /* end before begin */
    CHECK_INT(r.code, 1); free(r.out);
    char *t = content_text(); CHECK_STR(t, "one\ntwo\n"); free(t);

    expect_ok(CLI("edit", "1", "1", "1", "4", "ONE"));
    msleep(30);
    tmp_write("guard.txt", "changed elsewhere\n", 18);                         /* someone else edits the file meanwhile */
    r = CLI("write");
    CHECK_INT(r.code, 1); free(r.out);
    e = post_json("/cmd", "{\"cmd\":\"write\"}"); CHECK_CONTAINS(json_gets(e, "error", NULL), "changed on disk"); json_free(e);
    size_t dl; char *disk = read_file(f, &dl); CHECK_STR(disk, "changed elsewhere\n"); free(disk);      /* untouched */
    expect_ok(CLI("write", "-f"));
    disk = read_file(f, &dl); CHECK_STR(disk, "ONE\ntwo\n"); free(disk);
    CHECK_INT(content_dirty(), 0);
}

static void t_write_needs_a_file(void) {
    expect_ok(CLI_IN("some text\n", 10, "open", "-", "--no-window"));
    run_t r = CLI("write");
    CHECK_INT(r.code, 1); free(r.out);
    jv *e = post_json("/cmd", "{\"cmd\":\"write\"}"); CHECK_CONTAINS(json_gets(e, "error", NULL), "no file"); json_free(e);
    expect_ok(CLI("edit", "1", "1", "1", "5", "SOME"));                        /* editing in memory is still allowed */
    char *t = content_text(); CHECK_STR(t, "SOME text\n"); free(t);
}

static void t_redo(void) {
    char *f = tmp_write("redo.txt", "alpha\nbeta\n", 11);
    expect_ok(CLI("open", f, "--no-window"));
    expect_ok(CLI("edit", "1", "1", "1", "6", "ALPHA"));
    expect_ok(CLI("edit", "2", "1", "2", "5", "BETA!"));
    expect_ok(CLI("undo")); expect_ok(CLI("undo"));
    char *t = content_text(); CHECK_STR(t, "alpha\nbeta\n"); free(t);
    expect_ok(CLI("redo"));
    t = content_text(); CHECK_STR(t, "ALPHA\nbeta\n"); free(t);
    expect_ok(CLI("redo"));
    t = content_text(); CHECK_STR(t, "ALPHA\nBETA!\n"); free(t);
    run_t r = CLI("redo"); CHECK_INT(r.code, 1); free(r.out);                     /* nothing left to redo */
    expect_ok(CLI("undo"));                                                       /* and a new edit empties the redo stack */
    expect_ok(CLI("edit", "2", "1", "2", "5", "new"));
    r = CLI("redo"); CHECK_INT(r.code, 1); free(r.out);
    t = content_text(); CHECK_STR(t, "ALPHA\nnew\n"); free(t);
    expect_ok(CLI("undo")); expect_ok(CLI("undo")); expect_ok(CLI("redo"));       /* redo still lines up after undoing past an insertion */
    t = content_text(); CHECK_STR(t, "ALPHA\nbeta\n"); free(t);
}

static void t_unsaved_changes_are_not_discarded(void) {
    char *f1 = tmp_write("keep1.txt", "one\ntwo\n", 8), *f2 = tmp_write("keep2.txt", "other\n", 6);
    expect_ok(CLI("open", f1, "--no-window"));
    expect_ok(CLI("edit", "1", "1", "1", "4", "ONE"));                         /* now there are unsaved edits */
    run_t r = CLI("open", f2, "--no-window");                                  /* opening another file would discard them */
    CHECK_INT(r.code, 1); free(r.out);
    jv *c = get_json("/content"); CHECK_STR(json_gets(c, "name", NULL), "keep1.txt"); CHECK_STR(json_gets(c, "text", NULL), "ONE\ntwo\n"); json_free(c);
    r = CLI("stop"); CHECK_INT(r.code, 1); free(r.out);                        /* and so would stopping the server */
    json_free(get_json("/status"));                                            /* the server is still there */
    expect_ok(CLI("write"));                                                   /* once written there is nothing to lose */
    expect_ok(CLI("open", f2, "--no-window"));
    c = get_json("/content"); CHECK_STR(json_gets(c, "name", NULL), "keep2.txt"); json_free(c);
    expect_ok(CLI("edit", "1", "1", "1", "6", "OTHER"));
    expect_ok(CLI("open", f1, "--no-window", "--force"));                      /* --force discards on purpose */
    c = get_json("/content"); CHECK_STR(json_gets(c, "name", NULL), "keep1.txt"); json_free(c);
}

/* ---- one server per window ---- */
static int port_in(const char *list, const char *name) {          /* the port on the line of `winless windows` that names this file */
    for (const char *p = list; p && *p;) {
        const char *eol = strchr(p, '\n'); size_t n = eol ? (size_t)(eol - p) : strlen(p);
        char line[1024]; if (n >= sizeof line) n = sizeof line - 1; memcpy(line, p, n); line[n] = 0;
        int port;
        if (strstr(line, name) && sscanf(strstr(line, "port ") ? strstr(line, "port ") : "", "port %d", &port) == 1) return port;
        p = eol ? eol + 1 : NULL;
    }
    return 0;
}
static int count_windows(const char *list) { int n = 0; for (const char *p = list; (p = strstr(p, "port ")); p++) n++; return n; }

static void t_each_open_has_its_own_server(void) {
    char *a = tmp_write("wa.txt", "aaa\n", 4), *b = tmp_write("wb.txt", "bbb\n", 4);
    run_t w = CLI_AUTO("windows"); CHECK_CONTAINS(w.out, "no windows"); free(w.out);
    run_t r = CLI_AUTO("open", a, "--no-window"); CHECK_INT(r.code, 0); free(r.out);
    r = CLI_AUTO("open", b, "--no-window"); CHECK_INT(r.code, 0); free(r.out);       /* a second file: a second server, the first is untouched */
    w = CLI_AUTO("windows");
    CHECK_INT(count_windows(w.out), 2);
    int pa = port_in(w.out, "wa.txt"), pb = port_in(w.out, "wb.txt");
    free(w.out);
    CHECK(pa > 0 && pb > 0 && pa != pb);
    g_port = pa; jv *c = get_json("/content"); CHECK_STR(json_gets(c, "name", NULL), "wa.txt"); CHECK_STR(json_gets(c, "text", NULL), "aaa\n"); json_free(c);
    g_port = pb; c = get_json("/content"); CHECK_STR(json_gets(c, "name", NULL), "wb.txt"); json_free(c);
    g_port = pa; expect_ok(CLI("edit", "1", "1", "1", "4", "AAA"));                   /* an edit in one does not touch the other */
    g_port = pb; CHECK_INT(content_dirty(), 0); g_port = pa; CHECK_INT(content_dirty(), 1);
    expect_ok(CLI("write"));
}

static void t_open_does_not_duplicate_a_file(void) {
    char *a = tmp_write("dup.txt", "x\n", 2);
    expect_ok(CLI_AUTO("open", a, "--no-window"));
    run_t r = CLI_AUTO("open", a, "--no-window");                                    /* already open: nothing new is started */
    CHECK_INT(r.code, 0); CHECK_CONTAINS(r.out, "already open"); free(r.out);
    run_t w = CLI_AUTO("windows"); CHECK_INT(count_windows(w.out), 1); free(w.out);
    expect_ok(CLI_AUTO("open", a, "--no-window", "--new"));                          /* unless asked */
    w = CLI_AUTO("windows"); CHECK_INT(count_windows(w.out), 2); free(w.out);
}

static void t_several_windows_means_choose(void) {
    char *a = tmp_write("ca.txt", "a\n", 2), *b = tmp_write("cb.txt", "b\n", 2);
    expect_ok(CLI_AUTO("open", a, "--no-window"));
    expect_ok(CLI_AUTO("goto", "1"));                                                /* one window: no question */
    expect_ok(CLI_AUTO("open", b, "--no-window"));
    run_t r = CLI_AUTO("goto", "1");                                                 /* two windows and no terminal: it lists them and refuses */
    CHECK_INT(r.code, 2); free(r.out);
    r = CLI_AUTO("status", "--json"); CHECK_INT(r.code, 2); free(r.out);
    run_t w = CLI_AUTO("windows"); int pb = port_in(w.out, "cb.txt"); free(w.out);
    g_port = pb;
    r = CLI("status", "--json"); CHECK_INT(r.code, 0); CHECK_CONTAINS(r.out, "cb.txt"); free(r.out);     /* --port picks one */
}

static void t_here_replaces_the_one_window(void) {
    char *a = tmp_write("ha.txt", "a\n", 2), *b = tmp_write("hb.txt", "b\n", 2);
    expect_ok(CLI_AUTO("open", a, "--no-window"));
    expect_ok(CLI_AUTO("open", b, "--here", "--no-window"));                         /* the old behaviour, on request */
    run_t w = CLI_AUTO("windows"); CHECK_INT(count_windows(w.out), 1); CHECK_CONTAINS(w.out, "hb.txt"); free(w.out);
}

static void t_stop_all_keeps_unsaved_work(void) {
    char *a = tmp_write("sa.txt", "a\n", 2), *b = tmp_write("sb.txt", "b\n", 2);
    expect_ok(CLI_AUTO("open", a, "--no-window")); expect_ok(CLI_AUTO("open", b, "--no-window"));
    run_t w = CLI_AUTO("windows"); int pa = port_in(w.out, "sa.txt"); free(w.out);
    g_port = pa; expect_ok(CLI("edit", "1", "1", "1", "2", "A"));                    /* unsaved edits in one of them */
    run_t r = CLI_AUTO("stop", "--all"); CHECK_INT(r.code, 1); free(r.out);          /* refused for that one ... */
    w = CLI_AUTO("windows"); CHECK_INT(count_windows(w.out), 1); CHECK_CONTAINS(w.out, "sa.txt"); CHECK_CONTAINS(w.out, "[unsaved]"); free(w.out);   /* ... the clean one stopped */
    expect_ok(CLI_AUTO("stop", "--all", "--force"));
    msleep(300);
    w = CLI_AUTO("windows"); CHECK_CONTAINS(w.out, "no windows"); free(w.out);
}

static void wait_for_ping(void) {                                   /* a server just spawned: wait until it answers */
    for (int i = 0; i < 100; i++) { resp_t r = http("GET", "/ping", NULL, 0); int ok = r.status == 200; free(r.body); if (ok) return; msleep(50); }
    FAIL("the server did not start");
}

static void t_quitting_discards_unsaved_edits(void) {
    /* a server made for a window ends with it: when the window quits, or a while after it is gone, even with unsaved edits */
    char *f = tmp_write("quitme.txt", "one\n", 4);
    g_port = free_port();
    char *serve[] = { "serve", "--exit-when-idle", "600", NULL };
    int out; pid_t pid = cli_spawn(serve, &out);
    wait_for_ping();
    expect_ok(CLI("open", f, "--no-window"));
    expect_ok(CLI("edit", "1", "1", "1", "4", "ONE"));
    CHECK_INT(content_dirty(), 1);
    json_free(post_json("/event", "{\"type\":\"quit\"}"));                 /* the window says :q! */
    run_t r = cli_finish(pid, out, 20.0);                                  /* the server ends by itself, soon */
    CHECK_INT(r.code, 0); free(r.out);
    size_t n; char *disk = read_file(f, &n); CHECK_STR(disk, "one\n"); free(disk);   /* the file was never written */
}

static void t_idle_server_with_unsaved_edits_exits_too(void) {
    char *f = tmp_write("goneme.txt", "one\n", 4);
    g_port = free_port();
    char *serve[] = { "serve", "--exit-when-idle", "2", NULL };
    int out; pid_t pid = cli_spawn(serve, &out);
    wait_for_ping();
    expect_ok(CLI("open", f, "--no-window"));
    expect_ok(CLI("edit", "1", "1", "1", "4", "ONE"));                     /* unsaved, and no window connected */
    run_t r = cli_finish(pid, out, 30.0);
    CHECK_INT(r.code, 0); free(r.out);
}

static void t_idle_server_exits(void) {
    g_port = free_port();
    run_t r = CLI("serve", "--exit-when-idle", "1");           /* no window ever connects and nothing is unsaved: it quits by itself */
    CHECK_INT(r.code, 0); free(r.out);
    run_t w = CLI_AUTO("windows"); CHECK_CONTAINS(w.out, "no windows"); free(w.out);      /* and removes its record */
}

static size_t history_count(void) {
    jv *j = post_json("/cmd", "{\"cmd\":\"history\"}");
    const jv *items = json_get(j, "items"); size_t n = items ? items->n : 0;
    json_free(j); return n;
}
static void check_disk(const char *f, const char *want) { size_t n; char *d = read_file(f, &n); CHECK_STR(d, want); free(d); }

static void t_every_write_can_be_reverted(void) {
    char *f = tmp_write("rev.txt", "v0\n", 3);
    expect_ok(CLI("open", f, "--no-window"));
    CHECK_INT(history_count(), 0);
    run_t r = CLI("revert"); CHECK_INT(r.code, 1); free(r.out);                   /* nothing kept yet */
    expect_ok(CLI("edit", "1", "1", "1", "3", "v1")); expect_ok(CLI("write"));    /* this write keeps v0 */
    expect_ok(CLI("edit", "1", "1", "1", "3", "v2")); expect_ok(CLI("write"));    /* this one keeps v1 */
    CHECK_INT(history_count(), 2);
    expect_ok(CLI("write"));                                                      /* writing what is already there keeps nothing new */
    CHECK_INT(history_count(), 2);
    r = CLI("history"); CHECK_INT(r.code, 0); CHECK_CONTAINS(r.out, "bytes"); free(r.out);

    expect_ok(CLI("revert"));                                                     /* put back the latest kept version: v1 */
    check_disk(f, "v1\n"); char *t = content_text(); CHECK_STR(t, "v1\n"); free(t); CHECK_INT(content_dirty(), 0);
    CHECK_INT(history_count(), 3);                                                /* the revert kept v2, so it can be reverted */
    expect_ok(CLI("revert", "3")); check_disk(f, "v2\n");
    expect_ok(CLI("revert", "1")); check_disk(f, "v0\n");                         /* all the way back to the original */
    r = CLI("revert", "99"); CHECK_INT(r.code, 1); free(r.out); check_disk(f, "v0\n");   /* no such version: nothing changes */

    expect_ok(CLI("edit", "1", "1", "1", "3", "new"));                            /* unsaved edits are not silently replaced */
    r = CLI("revert"); CHECK_INT(r.code, 1); free(r.out);
    t = content_text(); CHECK_STR(t, "new\n"); free(t);
    expect_ok(CLI("write"));
}

static void t_overwriting_someone_elses_change_is_recoverable(void) {
    char *f = tmp_write("theirs.txt", "mine\n", 5);
    expect_ok(CLI("open", f, "--no-window"));
    msleep(30);
    tmp_write("theirs.txt", "someone else's edit\n", 20);                         /* the file changes behind winless's back */
    expect_ok(CLI("edit", "1", "1", "1", "5", "MINE"));
    run_t r = CLI("write"); CHECK_INT(r.code, 1); free(r.out);                    /* refused ... */
    expect_ok(CLI("write", "-f"));                                                /* ... and when forced, their version is kept */
    check_disk(f, "MINE\n");
    expect_ok(CLI("revert"));
    check_disk(f, "someone else's edit\n");
}

/* ---- annotations ---- */
static jv *annotation_list(int deleted) {
    char body[80]; snprintf(body, sizeof body, "{\"cmd\":\"annotations\"%s}", deleted ? ",\"deleted\":true" : "");
    return post_json("/cmd", body);
}
static const jv *find_note(const jv *list, const char *note) {         /* the item with this note text */
    const jv *items = json_get(list, "items");
    for (size_t i = 0; items && i < items->n; i++) if (!strcmp(json_gets(items->items[i], "note", NULL), note)) return items->items[i];
    return NULL;
}

static void t_annotations_by_scope(void) {
    char *f = tmp_write("ann.txt", "alpha beta\n\ngamma delta\nepsilon\n", 31);
    expect_ok(CLI("open", f, "--no-window"));
    run_t r = CLI("annotations"); CHECK_CONTAINS(r.out, "no notes"); free(r.out);

    expect_ok(CLI("annotate", "1", "7", "1", "11", "about beta"));            /* inside one paragraph: the text is kept */
    expect_ok(CLI("annotate", "3", "1", "5", "1", "two paragraphs"));         /* rows 3 and 4 (an end at the start of row 5 does not reach row 5) */
    expect_ok(CLI("annotate", "--file", "the whole thing"));
    expect_ok(CLI("annotate", "1", "1", "5", "1", "all of it, selected"));    /* everything selected counts as the file */
    expect_ok(CLI("annotate", "4", "2", "4", "5", "multi\xC3\xA9 byte"));      /* columns count code points */

    jv *l = annotation_list(0);
    const jv *n1 = find_note(l, "about beta");
    CHECK(n1 != NULL); CHECK_STR(json_gets(n1, "scope", NULL), "text"); CHECK_STR(json_gets(n1, "text", NULL), "beta");
    CHECK_INT(json_getnum(n1, "r1", 0), 1); CHECK_INT(json_getnum(n1, "c1", 0), 7); CHECK_INT(json_getnum(n1, "r2", 0), 1); CHECK_INT(json_getnum(n1, "c2", 0), 11);
    CHECK_STR(json_gets(n1, "file", NULL), f);                                  /* the file's real path */
    CHECK(strlen(json_gets(n1, "created", NULL)) == 20 && json_gets(n1, "created", NULL)[10] == 'T');
    const jv *n2 = find_note(l, "two paragraphs");
    CHECK(n2 != NULL); CHECK_STR(json_gets(n2, "scope", NULL), "paragraphs"); CHECK_STR(json_gets(n2, "paragraphs", NULL), "3-4");
    CHECK(json_get(n2, "text")->t == J_NULL);                                  /* more than a paragraph: no text kept */
    const jv *n3 = find_note(l, "the whole thing");
    CHECK(n3 != NULL); CHECK_STR(json_gets(n3, "scope", NULL), "file"); CHECK(json_get(n3, "r1")->t == J_NULL);
    const jv *n4 = find_note(l, "all of it, selected");
    CHECK(n4 != NULL); CHECK_STR(json_gets(n4, "scope", NULL), "file");
    const jv *n5 = find_note(l, "multi\xC3\xA9 byte"); CHECK(n5 != NULL); CHECK_STR(json_gets(n5, "text", NULL), "psi");
    json_free(l);

    r = CLI("annotate", "1", "1", "1", "1", "empty selection"); CHECK_INT(r.code, 1); free(r.out);     /* nothing selected */
    r = CLI("annotate", "9", "1", "9", "5", "off the end"); CHECK_INT(r.code, 1); free(r.out);
    r = CLI("annotate", "1", "1", "1", "5", ""); CHECK_INT(r.code, 1); free(r.out);                      /* an empty note */

    jv *c = get_json("/content");                                               /* the page gets the notes with the text */
    const jv *an = json_get(c, "annotations"); CHECK(an != NULL && an->t == J_ARR && an->n == 5); json_free(c);

    long id = (long)json_getnum(find_note((l = annotation_list(0)), "about beta"), "id", 0); json_free(l);
    char ids[24]; snprintf(ids, sizeof ids, "%ld", id);
    expect_ok(CLI("unannotate", ids));                                          /* hidden, not deleted ... */
    l = annotation_list(0); CHECK(find_note(l, "about beta") == NULL); json_free(l);
    l = annotation_list(1); CHECK(find_note(l, "about beta") != NULL); json_free(l);
    expect_ok(CLI("unannotate", "--restore", ids));                             /* ... and it comes back */
    l = annotation_list(0); CHECK(find_note(l, "about beta") != NULL); json_free(l);
    r = CLI("unannotate", "999999"); CHECK_INT(r.code, 1); free(r.out);
}

static void t_annotation_kinds(void) {
    char *f = tmp_write("kinds.txt", "alpha beta gamma\n\ndelta epsilon\n", 31);
    expect_ok(CLI("open", f, "--no-window"));
    expect_ok(CLI("annotate", "--kind", "slop", "1", "1", "1", "6"));                      /* slop and keep need no words */
    expect_ok(CLI("annotate", "--kind", "keep", "1", "7", "1", "11", "this is good"));
    expect_ok(CLI("annotate", "--kind", "slop", "3", "1", "3", "6", "too flowery"));
    expect_ok(CLI("annotate", "1", "12", "1", "17", "cut this"));                           /* the default kind is note */
    run_t r = CLI("annotate", "1", "12", "1", "17"); CHECK_INT(r.code, 1); free(r.out);      /* a plain note needs its words */
    r = CLI("annotate", "--kind", "nonsense", "1", "1", "1", "6", "x"); CHECK_INT(r.code, 1); free(r.out);

    jv *l = annotation_list(0);
    const jv *items = json_get(l, "items");
    int slop = 0, keep = 0, note = 0;
    for (size_t i = 0; items && i < items->n; i++) {
        const jv *it = items->items[i];
        CHECK_STR(json_gets(it, "status", NULL), "open");
        const char *k = json_gets(it, "kind", NULL);
        if (!strcmp(k, "slop")) slop++; else if (!strcmp(k, "keep")) keep++; else if (!strcmp(k, "note")) note++;
    }
    CHECK_INT(slop, 2); CHECK_INT(keep, 1); CHECK_INT(note, 1);
    const jv *one = find_note(l, "this is good"); CHECK(one != NULL); CHECK_STR(json_gets(one, "kind", NULL), "keep"); CHECK_STR(json_gets(one, "text", NULL), "beta");
    json_free(l);
    run_t ls = CLI("annotations"); CHECK_CONTAINS(ls.out, "slop"); CHECK_CONTAINS(ls.out, "keep"); CHECK_CONTAINS(ls.out, "note"); free(ls.out);
}

static void t_old_annotation_database_is_upgraded(void) {
    if (access("/usr/bin/sqlite3", X_OK) != 0 && access("/bin/sqlite3", X_OK) != 0) return;       /* needs the sqlite3 tool to make the old file */
    char dir[900], cmd[4096];
    snprintf(dir, sizeof dir, "%s/home/.local/share/winless", tmp_dir());
    snprintf(cmd, sizeof cmd, "rm -f '%s/winless.db' && sqlite3 '%s/winless.db' \"CREATE TABLE annotations (id INTEGER PRIMARY KEY AUTOINCREMENT, "
             "file TEXT NOT NULL, created TEXT NOT NULL, row_beg INTEGER, col_beg INTEGER, row_end INTEGER, col_end INTEGER, "
             "scope TEXT NOT NULL CHECK (scope IN ('text','paragraphs','file')), selected_text TEXT, paragraphs TEXT, note TEXT NOT NULL, "
             "deleted INTEGER NOT NULL DEFAULT 0); INSERT INTO annotations (file, created, row_beg, col_beg, row_end, col_end, scope, selected_text, note) "
             "VALUES ('/old/file.md', '2026-01-01T00:00:00Z', 1, 1, 1, 4, 'text', 'word', 'an old note');\"", dir, dir);
    CHECK_INT(system(cmd), 0);
    char *f = tmp_write("up.txt", "word and more\n", 14);
    expect_ok(CLI("open", f, "--no-window"));
    jv *l = post_json("/cmd", "{\"cmd\":\"annotations\",\"all\":true}");
    const jv *old = find_note(l, "an old note");
    CHECK(old != NULL); CHECK_STR(json_gets(old, "kind", NULL), "note"); CHECK_STR(json_gets(old, "status", NULL), "open");     /* the old note survived, as an open note */
    json_free(l);
    expect_ok(CLI("annotate", "--kind", "slop", "1", "1", "1", "5"));                                                          /* and the upgraded database takes new kinds */
    l = annotation_list(0); CHECK_INT(json_get(l, "items")->n, 1); json_free(l);
}

/* ---- filters ---- */
static jv *filter_json(long r1, long c1, long r2, long c2, const char *command, int lines, double timeout) {
    sb_t b; sb_init(&b);
    sb_printf(&b, "{\"cmd\":\"filter\",\"r1\":%ld,\"c1\":%ld,\"r2\":%ld,\"c2\":%ld,\"linewise\":%s,\"timeout\":%g,\"command\":", r1, c1, r2, c2, lines ? "true" : "false", timeout);
    json_put_string(&b, command, strlen(command)); sb_putc(&b, '}');
    jv *j = post_json("/cmd", b.s); sb_free(&b); return j;
}

static void t_filters(void) {
    char *f = tmp_write("filt.txt", "banana\napple\ncherry\nna\xC3\xAFve\n", 27);
    expect_ok(CLI("open", f, "--no-window"));
    expect_ok(CLI("filter", "1", "1", "4", "1", "--lines", "sort"));          /* whole lines through sort */
    char *t = content_text(); CHECK_STR(t, "apple\nbanana\ncherry\nna\xC3\xAFve\n"); free(t);
    expect_ok(CLI("undo")); t = content_text(); CHECK_STR(t, "banana\napple\ncherry\nna\xC3\xAFve\n"); free(t);   /* one step undoes it */

    expect_ok(CLI("filter", "1", "1", "1", "7", "tr a-z A-Z"));               /* exactly the characters in the range */
    t = content_text(); CHECK_STR(t, "BANANA\napple\ncherry\nna\xC3\xAFve\n"); free(t);
    expect_ok(CLI("undo"));
    expect_ok(CLI("filter", "4", "1", "4", "6", "cat"));                      /* multibyte text passes through unharmed (and nothing changes) */
    t = content_text(); CHECK_STR(t, "banana\napple\ncherry\nna\xC3\xAFve\n"); free(t);
    CHECK_INT(content_dirty(), 0);

    jv *j = filter_json(2, 1, 3, 1, "printf apple-without-newline", 1, 10);   /* whole lines stay whole lines: a newline is added */
    CHECK_INT(json_getbool(j, "ok", 0), 1); json_free(j);
    t = content_text(); CHECK_STR(t, "banana\napple-without-newline\ncherry\nna\xC3\xAFve\n"); free(t);
    expect_ok(CLI("undo"));

    j = filter_json(1, 1, 2, 1, "echo oops >&2; exit 3", 1, 10);              /* a failing command changes nothing and says why */
    CHECK_INT(json_getbool(j, "ok", 1), 0); CHECK_CONTAINS(json_gets(j, "error", NULL), "exit 3"); CHECK_CONTAINS(json_gets(j, "error", NULL), "oops"); json_free(j);
    t = content_text(); CHECK_STR(t, "banana\napple\ncherry\nna\xC3\xAFve\n"); free(t);
    CHECK_INT(content_dirty(), 0);

    j = filter_json(1, 1, 2, 1, "no-such-command-anywhere", 1, 10);           /* a missing command is a failure too */
    CHECK_INT(json_getbool(j, "ok", 1), 0); json_free(j);

    double t0 = 0; { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); t0 = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9; }
    j = filter_json(1, 1, 2, 1, "sleep 20", 1, 0.5);                          /* a command that hangs is stopped */
    { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); double took = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9 - t0; CHECK(took < 5.0); }
    CHECK_INT(json_getbool(j, "ok", 1), 0); CHECK_CONTAINS(json_gets(j, "error", NULL), "in time"); json_free(j);

    j = filter_json(1, 1, 1, 1, "pwd", 0, 10);                                /* it runs in the file's directory, and knows which file */
    CHECK_INT(json_getbool(j, "ok", 0), 1); json_free(j);
    t = content_text(); { char want[1200]; const char *sl = strrchr(f, '/'); snprintf(want, sizeof want, "%.*s\nbanana\napple\ncherry\nna\xC3\xAFve\n", (int)(sl - f), f); CHECK_STR(t, want); } free(t);
    expect_ok(CLI("undo"));
    j = filter_json(1, 1, 1, 1, "printenv WINLESS_FILE", 0, 10); json_free(j);
    t = content_text(); CHECK_CONTAINS(t, "filt.txt\nbanana"); free(t);
    expect_ok(CLI("undo"));

    j = filter_json(9, 1, 9, 1, "cat", 0, 10); CHECK_INT(json_getbool(j, "ok", 1), 0); json_free(j);        /* a bad range */
}

static void t_filters_with_big_input_and_output(void) {
    sb_t big; sb_init(&big);
    for (int i = 0; i < 20000; i++) sb_printf(&big, "line number %d of a long file\n", i);        /* about 600 KB */
    char *f = tmp_write("big.txt", big.s, big.len);
    expect_ok(CLI("open", f, "--no-window"));
    jv *j = filter_json(1, 1, 20001, 1, "echo done", 1, 20);                  /* ignores its input: no deadlock while we write it */
    CHECK_INT(json_getbool(j, "ok", 0), 1); json_free(j);
    char *t = content_text(); CHECK_STR(t, "done\n"); free(t);
    expect_ok(CLI("undo"));
    j = filter_json(1, 1, 20001, 1, "sed p", 1, 20);                           /* input and output both bigger than a pipe buffer */
    CHECK_INT(json_getbool(j, "ok", 0), 1); json_free(j);
    t = content_text(); CHECK_INT(strlen(t), big.len * 2); free(t);
    sb_free(&big);
}

int main(void) {
    testlib_init();
    static const testcase tests[] = {
        { "open_status_goto_key", t_open_status_goto_key, 1 },
        { "open_from_stdin_and_cleans_invalid_utf8", t_open_from_stdin_and_cleans_invalid_utf8, 1 },
        { "open_options_and_line", t_open_options_and_line, 1 },
        { "mark_hl_search_config_events", t_mark_hl_search_config_events, 1 },
        { "simple_commands_and_variants", t_simple_commands_and_variants, 1 },
        { "status_without_a_window", t_status_without_a_window, 1 },
        { "rejects_bad_arguments", t_rejects_bad_arguments, 1 },
        { "wait_returns_when_quit_arrives", t_wait_returns_when_quit_arrives, 1 },
        { "wait_times_out_with_status_1", t_wait_times_out_with_status_1, 1 },
        { "open_wait_blocks_until_the_window_closes", t_open_wait_blocks_until_the_window_closes, 1 },
        { "cli_starts_and_stops_its_own_server", t_cli_starts_and_stops_its_own_server, 0 },
        { "any_command_autostarts_the_server", t_any_command_autostarts_the_server, 0 },
        { "version_and_help", t_version_and_help, 0 },
        { "edit_undo_write", t_edit_undo_write, 1 },
        { "edit_rejects_bad_ranges_and_changed_files", t_edit_rejects_bad_ranges_and_changed_files, 1 },
        { "write_needs_a_file", t_write_needs_a_file, 1 },
        { "redo", t_redo, 1 },
        { "annotations_by_scope", t_annotations_by_scope, 1 },
        { "annotation_kinds", t_annotation_kinds, 1 },
        { "old_annotation_database_is_upgraded", t_old_annotation_database_is_upgraded, 1 },
        { "filters", t_filters, 1 },
        { "filters_with_big_input_and_output", t_filters_with_big_input_and_output, 1 },
        { "every_write_can_be_reverted", t_every_write_can_be_reverted, 1 },
        { "overwriting_someone_elses_change_is_recoverable", t_overwriting_someone_elses_change_is_recoverable, 1 },
        { "unsaved_changes_are_not_discarded", t_unsaved_changes_are_not_discarded, 1 },
        { "each_open_has_its_own_server", t_each_open_has_its_own_server, 0 },
        { "open_does_not_duplicate_a_file", t_open_does_not_duplicate_a_file, 0 },
        { "several_windows_means_choose", t_several_windows_means_choose, 0 },
        { "here_replaces_the_one_window", t_here_replaces_the_one_window, 0 },
        { "stop_all_keeps_unsaved_work", t_stop_all_keeps_unsaved_work, 0 },
        { "idle_server_exits", t_idle_server_exits, 0 },
        { "quitting_discards_unsaved_edits", t_quitting_discards_unsaved_edits, 0 },
        { "idle_server_with_unsaved_edits_exits_too", t_idle_server_with_unsaved_edits_exits_too, 0 },
    };
    return run_tests(tests, sizeof tests / sizeof *tests) ? 1 : 0;
}
