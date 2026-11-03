/* test_cli.c -- black-box tests of the winless command line against a real server process.
 * Copyright (C) 2026 David Walther.  GPL-3.0-or-later. */
#define _POSIX_C_SOURCE 200809L
#include "testlib.h"
#include <signal.h>
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
    };
    return run_tests(tests, sizeof tests / sizeof *tests) ? 1 : 0;
}
