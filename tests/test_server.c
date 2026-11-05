/* test_server.c -- black-box tests of the winless HTTP/SSE server (the binary is started as a separate process).
 * Copyright (C) 2026 David Walther.  GPL-3.0-or-later. */
#define _POSIX_C_SOURCE 200809L
#include "testlib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static jv *post_json(const char *path, const char *body) {
    char *r = post_ok(path, body);
    jv *j = json_parse(r, strlen(r));
    if (!j) FAIL("POST %s returned invalid JSON: %.200s", path, r);
    free(r);
    return j;
}

static long status_seq(void) { jv *j = get_json("/status"); long n = (long)json_getnum(j, "seq", -1); json_free(j); return n; }

static void t_ping_and_version(void) {
    jv *j = get_json("/ping");
    CHECK_INT(json_getbool(j, "ok", 0), 1);
    CHECK_STR(json_gets(j, "version", NULL), PACKAGE_VERSION);
    json_free(j);
}

static void t_viewer_page_is_served(void) {
    char *html = get_ok("/");
    CHECK_CONTAINS(html, "<title>winless</title>");
    CHECK_CONTAINS(html, "EventSource");
    free(html);
}

static void t_unknown_path_and_bad_json(void) {
    resp_t r = http("GET", "/nope", NULL, 0);
    CHECK_INT(r.status, 404);
    free(r.body);
    CHECK_INT(post_status("/cmd", "{not json"), 400);
    CHECK_INT(post_status("/cmd", "{\"no_cmd\":1}"), 400);
    CHECK_INT(post_status("/event", "garbage"), 400);
    resp_t p = http("PUT", "/cmd", "{}", 2);
    CHECK_INT(p.status, 404);
    free(p.body);
}

static void t_load_and_content_roundtrip_with_hebrew_and_escapes(void) {
    const char *esc = "line one \\\"quoted\\\" \\\\ back\\nיְהֹוָה אֵל רַחוּם\\ttab \xF0\x9F\x98\x80 emoji\\nlast";
    const char *want = "line one \"quoted\" \\ back\nיְהֹוָה אֵל רַחוּם\ttab \xF0\x9F\x98\x80 emoji\nlast";
    sb_t b; sb_init(&b);
    sb_printf(&b, "{\"cmd\":\"load\",\"name\":\"demo.txt\",\"text\":\"%s\",\"line\":2,\"search\":\"last\","
                  "\"settings\":{\"theme\":\"light\",\"numbers\":true,\"font\":null}}", esc);
    jv *r = post_json("/cmd", b.s);
    sb_free(&b); json_free(r);
    jv *c = get_json("/content");
    CHECK_STR(json_gets(c, "name", NULL), "demo.txt");
    CHECK_STR(json_gets(c, "text", NULL), want);
    CHECK_INT(json_getnum(c, "top", 0), 2);
    CHECK_STR(json_gets(c, "search", NULL), "last");
    CHECK_INT(json_getnum(c, "version", 0), 1);
    const jv *st = json_get(c, "settings");
    CHECK_STR(json_gets(st, "theme", NULL), "light");
    CHECK_INT(json_getbool(st, "numbers", 0), 1);
    CHECK(json_get(st, "font") == NULL);                      /* null settings are not stored */
    json_free(c);
}

static void t_surrogate_pair_escape_is_decoded(void) {
    jv *r = post_json("/cmd", "{\"cmd\":\"load\",\"name\":\"s\",\"text\":\"x\\ud83d\\ude00y\\u0041\"}");
    json_free(r);
    jv *c = get_json("/content");
    CHECK_STR(json_gets(c, "text", NULL), "x\xF0\x9F\x98\x80yA");
    json_free(c);
}

static void t_large_text_roundtrip(void) {
    sb_t text; sb_init(&text);
    for (int i = 0; i < 40000; i++) sb_printf(&text, "line %d xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n", i);
    sb_t body; sb_init(&body);
    sb_puts(&body, "{\"cmd\":\"load\",\"name\":\"big\",\"text\":");
    json_put_string(&body, text.s, text.len);
    sb_putc(&body, '}');
    resp_t r = http("POST", "/cmd", body.s, body.len);
    CHECK_INT(r.status, 200);
    free(r.body);
    jv *c = get_json("/content");
    size_t n = 0;
    const char *got = json_gets(c, "text", &n);
    CHECK(got != NULL && n == text.len && memcmp(got, text.s, n) == 0);
    json_free(c); sb_free(&text); sb_free(&body);
}

static void t_commands_are_broadcast_to_the_page(void) {
    sse_t *page = sse_open();
    wait_clients(1);
    jv *r = post_json("/cmd", "{\"cmd\":\"goto\",\"line\":42}");
    CHECK_INT(json_getnum(r, "clients", -1), 1);
    json_free(r);
    json_free(post_json("/cmd", "{\"cmd\":\"search\",\"pattern\":\"needle\",\"backward\":false}"));
    CHECK_INT(sse_wait(page, 2, 5), 2);
    CHECK_STR(sse_event(page, 0), "{\"cmd\":\"goto\",\"line\":42}");
    CHECK_STR(sse_event(page, 1), "{\"cmd\":\"search\",\"pattern\":\"needle\",\"backward\":false}");
    sse_close(page);
}

static void t_load_broadcasts_a_bare_load_without_the_text(void) {
    sse_t *page = sse_open();
    wait_clients(1);
    sb_t b; sb_init(&b);
    sb_puts(&b, "{\"cmd\":\"load\",\"name\":\"n\",\"text\":\"");
    for (int i = 0; i < 1000; i++) sb_putc(&b, 'x');
    sb_puts(&b, "\"}");
    json_free(post_json("/cmd", b.s));
    sb_free(&b);
    CHECK_INT(sse_wait(page, 1, 5), 1);
    CHECK_STR(sse_event(page, 0), "{\"cmd\":\"load\"}");
    sse_close(page);
}

static void t_several_pages_all_receive_commands(void) {
    sse_t *a = sse_open(), *b = sse_open();
    wait_clients(2);
    json_free(post_json("/cmd", "{\"cmd\":\"flash\",\"text\":\"hi\"}"));
    CHECK_INT(sse_wait(a, 1, 5), 1);
    CHECK_INT(sse_wait(b, 1, 5), 1);
    CHECK_STR(sse_event(a, 0), "{\"cmd\":\"flash\",\"text\":\"hi\"}");
    CHECK_STR(sse_event(b, 0), "{\"cmd\":\"flash\",\"text\":\"hi\"}");
    sse_close(a); sse_close(b);
}

static void t_marks_and_highlights_persist_for_reconnects(void) {
    json_free(post_json("/cmd", "{\"cmd\":\"load\",\"name\":\"x\",\"text\":\"a\\nb\\nc\"}"));
    json_free(post_json("/cmd", "{\"cmd\":\"mark\",\"a\":1,\"b\":2,\"color\":\"yellow\"}"));
    json_free(post_json("/cmd", "{\"cmd\":\"mark\",\"a\":7,\"b\":7,\"color\":null}"));
    json_free(post_json("/cmd", "{\"cmd\":\"hl\",\"pattern\":\"b\",\"color\":\"blue\"}"));
    jv *c = get_json("/content");
    const jv *marks = json_get(c, "marks"), *hls = json_get(c, "hls");
    CHECK(marks && marks->n == 2 && hls && hls->n == 1);
    CHECK_INT(json_getnum(marks->items[0], "a", 0), 1); CHECK_INT(json_getnum(marks->items[0], "b", 0), 2);
    CHECK_STR(json_gets(marks->items[0], "color", NULL), "yellow");
    CHECK(json_get(marks->items[1], "color")->t == J_NULL);
    CHECK_STR(json_gets(hls->items[0], "pattern", NULL), "b"); CHECK_STR(json_gets(hls->items[0], "color", NULL), "blue");
    json_free(c);
    json_free(post_json("/cmd", "{\"cmd\":\"unmark\"}"));
    json_free(post_json("/cmd", "{\"cmd\":\"unhl\"}"));
    c = get_json("/content");
    CHECK_INT(json_get(c, "marks")->n, 0); CHECK_INT(json_get(c, "hls")->n, 0);
    json_free(c);
    json_free(post_json("/cmd", "{\"cmd\":\"load\",\"name\":\"y\",\"text\":\"z\"}"));       /* a new load clears them too */
    json_free(post_json("/cmd", "{\"cmd\":\"mark\",\"a\":1,\"b\":1}"));
    json_free(post_json("/cmd", "{\"cmd\":\"load\",\"name\":\"y\",\"text\":\"z\"}"));
    c = get_json("/content");
    CHECK_INT(json_get(c, "marks")->n, 0);
    json_free(c);
}

static void t_search_and_clear_are_remembered(void) {
    json_free(post_json("/cmd", "{\"cmd\":\"search\",\"pattern\":\"p1\"}"));
    jv *c = get_json("/content");
    CHECK_STR(json_gets(c, "search", NULL), "p1");
    json_free(c);
    json_free(post_json("/cmd", "{\"cmd\":\"clear\"}"));
    c = get_json("/content");
    CHECK(json_get(c, "search")->t == J_NULL);
    json_free(c);
}

static void t_page_state_is_readable_and_resumes(void) {
    json_free(post_json("/event", "{\"type\":\"state\",\"top\":17,\"bottom\":40,\"total\":600,\"visible\":[\"a\",\"b\"]}"));
    jv *s = get_json("/status");
    const jv *v = json_get(s, "view");
    CHECK_INT(json_getnum(v, "top", 0), 17); CHECK_INT(json_getnum(v, "total", 0), 600);
    CHECK_INT(json_get(v, "visible")->n, 2);
    json_free(s);
    jv *c = get_json("/content");
    CHECK_INT(json_getnum(c, "top", 0), 17);                  /* a reloaded page resumes where it was */
    json_free(c);
}

static void t_quit_event_releases_a_waiting_request(void) {
    char path[320]; snprintf(path, sizeof path, "GET /wait?since=%ld&timeout=10 HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Winless-Token: %s\r\nConnection: close\r\n\r\n", status_seq(), test_token());
    int fd = raw_connect();
    raw_send(fd, path, strlen(path));
    size_t n;
    char *early = raw_read(fd, 0.4, &n);                      /* nothing yet: it is blocked */
    CHECK_INT(n, 0); free(early);
    json_free(post_json("/event", "{\"type\":\"quit\"}"));
    char *reply = raw_read(fd, 5, &n);
    CHECK_CONTAINS(reply, "\"event\":\"quit\"");
    CHECK_CONTAINS(reply, "\"timeout\":false");
    free(reply);
}

static void t_wait_times_out(void) {
    char path[64]; snprintf(path, sizeof path, "/wait?since=%ld&timeout=0.4", status_seq());
    double t0 = now_monotonic();
    jv *r = get_json(path);
    double dt = now_monotonic() - t0;
    CHECK_INT(json_getbool(r, "timeout", 0), 1);
    CHECK(dt > 0.3 && dt < 3.0);
    json_free(r);
}

static void t_closed_event_also_releases_wait(void) {
    long seq = status_seq();
    json_free(post_json("/event", "{\"type\":\"closed\"}"));
    char path[64]; snprintf(path, sizeof path, "/wait?since=%ld&timeout=5", seq);
    jv *r = get_json(path);
    CHECK_STR(json_gets(r, "event", NULL), "closed");
    CHECK_INT(json_getbool(r, "timeout", 1), 0);
    json_free(r);
}

static void t_disconnected_page_is_dropped_at_once(void) {
    sse_t *page = sse_open();
    wait_clients(1);
    sse_close(page);
    for (int i = 0; i < 60 && clients() != 0; i++) msleep(50);
    CHECK_INT(clients(), 0);
}

static void t_garbage_and_slow_clients_do_not_hurt_the_server(void) {
    static const char *junk[] = { "\x00\x01\x02garbage\r\n\r\n", "GET", "\r\n\r\n", "POST /cmd HTTP/1.1\r\n\r\n",
                                  "GET /" "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa HTTP/1.1\r\n\r\n" };
    for (size_t i = 0; i < sizeof junk / sizeof *junk; i++) {
        int fd = raw_connect();
        raw_send(fd, junk[i], i == 0 ? 15 : strlen(junk[i]));
        raw_close(fd);
    }
    int fd = raw_connect();
    const char *huge = "POST /cmd HTTP/1.1\r\nContent-Length: 99999999999\r\n\r\n";
    raw_send(fd, huge, strlen(huge));
    size_t n; char *reply = raw_read(fd, 3, &n);
    CHECK_CONTAINS(reply, "400");
    free(reply); raw_close(fd);
    int half = raw_connect();                                 /* a client whose body never completes */
    const char *partial = "POST /cmd HTTP/1.1\r\nContent-Length: 20\r\n\r\n{\"cmd\"";
    raw_send(half, partial, strlen(partial));
    char *p = get_ok("/ping");
    CHECK_CONTAINS(p, "\"ok\":true"); free(p);
    raw_close(half);
    p = get_ok("/ping");
    CHECK_CONTAINS(p, "\"ok\":true"); free(p);
}

static void t_many_connections_at_once(void) {
    sse_t *pages[20];
    for (int i = 0; i < 20; i++) pages[i] = sse_open();
    wait_clients(20);
    json_free(post_json("/cmd", "{\"cmd\":\"flash\",\"text\":\"all\"}"));
    for (int i = 0; i < 20; i++) CHECK_INT(sse_wait(pages[i], 1, 5), 1);
    for (int i = 0; i < 20; i++) sse_close(pages[i]);
    for (int i = 0; i < 60 && clients() != 0; i++) msleep(50);
    CHECK_INT(clients(), 0);
}

static void t_stop_command_ends_the_server(void) {
    jv *r = post_json("/cmd", "{\"cmd\":\"stop\"}");
    CHECK_INT(json_getbool(r, "ok", 0), 1);
    json_free(r);
    CHECK(server_exited(5.0));
}

/* A web page from somewhere else, or a hostname rebound to 127.0.0.1, must not be able to use the server (it can edit, write and run filters). */
static char *raw_request_as(const char *method, const char *host, const char *origin, const char *body, const char *token) {
    char req[1024];
    int n = snprintf(req, sizeof req, "%s %s HTTP/1.1\r\n", method, body ? "/cmd" : "/ping");
    if (host) n += snprintf(req + n, sizeof req - (size_t)n, "Host: %s\r\n", host);
    if (origin) n += snprintf(req + n, sizeof req - (size_t)n, "Origin: %s\r\n", origin);
    if (token) n += snprintf(req + n, sizeof req - (size_t)n, "X-Winless-Token: %s\r\n", token);
    n += snprintf(req + n, sizeof req - (size_t)n, "Connection: close\r\nContent-Length: %zu\r\n\r\n%s", body ? strlen(body) : 0, body ? body : "");
    int fd = raw_connect(); raw_send(fd, req, (size_t)n);
    size_t len; char *reply = raw_read(fd, 5, &len); raw_close(fd);
    return reply;
}
static char *raw_request(const char *method, const char *host, const char *origin, const char *body) {
    return raw_request_as(method, host, origin, body, test_token());
}

static void t_only_local_pages_may_use_the_server(void) {
    char hostport[40]; snprintf(hostport, sizeof hostport, "localhost:%d", g_port);
    char origin_ok[60]; snprintf(origin_ok, sizeof origin_ok, "http://127.0.0.1:%d", g_port);
    char origin_wrongport[60]; snprintf(origin_wrongport, sizeof origin_wrongport, "http://127.0.0.1:%d", g_port + 1);
    const char *cmd = "{\"cmd\":\"flash\",\"text\":\"x\"}";
    char *r;
    r = raw_request("POST", hostport, origin_ok, cmd);            CHECK_CONTAINS(r, "200 OK"); free(r);          /* the page itself */
    r = raw_request("POST", "127.0.0.1", NULL, cmd);               CHECK_CONTAINS(r, "200 OK"); free(r);          /* the command line */
    r = raw_request("POST", NULL, NULL, cmd);                      CHECK_CONTAINS(r, "200 OK"); free(r);          /* an old client without Host */
    r = raw_request("POST", hostport, "http://evil.example", cmd); CHECK_CONTAINS(r, "403"); free(r);             /* another site's page */
    r = raw_request("POST", hostport, "null", cmd);                CHECK_CONTAINS(r, "403"); free(r);             /* a sandboxed page */
    r = raw_request("POST", hostport, origin_wrongport, cmd);      CHECK_CONTAINS(r, "403"); free(r);             /* another local service */
    r = raw_request("POST", "evil.example", NULL, cmd);            CHECK_CONTAINS(r, "403"); free(r);             /* DNS rebinding */
    r = raw_request("POST", "evil.example:80", origin_ok, cmd);    CHECK_CONTAINS(r, "403"); free(r);
    r = raw_request("GET", "evil.example", NULL, NULL);            CHECK_CONTAINS(r, "403"); free(r);             /* reads too */
    r = raw_request("GET", "localhost.evil.example", NULL, NULL);  CHECK_CONTAINS(r, "403"); free(r);             /* a longer name that starts like ours */
    r = raw_request("GET", "[::1]", NULL, NULL);                   CHECK_CONTAINS(r, "200 OK"); free(r);
}

/* Anyone who can reach the port (another user on a shared machine) must not be able to run filters: every request but /ping needs the secret. */
static char *raw_get(const char *path, const char *extra_headers) {
    char req[1024];
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: localhost:%d\r\n%sConnection: close\r\n\r\n", path, g_port, extra_headers);
    int fd = raw_connect(); raw_send(fd, req, (size_t)n);
    size_t len; char *reply = raw_read(fd, 5, &len); raw_close(fd);
    return reply;
}

static void t_the_secret_is_required(void) {
    const char *cmd = "{\"cmd\":\"flash\",\"text\":\"x\"}";
    char *r;
    r = raw_request_as("POST", "127.0.0.1", NULL, cmd, NULL);            CHECK_CONTAINS(r, "403"); free(r);       /* no secret */
    r = raw_request_as("POST", "127.0.0.1", NULL, cmd, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"); CHECK_CONTAINS(r, "403"); free(r);   /* wrong */
    r = raw_request_as("POST", "127.0.0.1", NULL, cmd, "short");         CHECK_CONTAINS(r, "403"); free(r);
    r = raw_request_as("GET", "127.0.0.1", NULL, NULL, NULL);            CHECK_CONTAINS(r, "200 OK"); free(r);    /* /ping needs none */
    r = raw_get("/status", "");                                          CHECK_CONTAINS(r, "403"); free(r);
    r = raw_get("/content", "");                                         CHECK_CONTAINS(r, "403"); free(r);
    r = raw_get("/", "");                                                CHECK_CONTAINS(r, "403"); free(r);
    r = raw_get("/?t=nope", "");                                         CHECK_CONTAINS(r, "403"); free(r);
    /* the window's way in: /?t=SECRET sets a cookie and sends it on to / ; the cookie then opens the rest */
    char path[160]; snprintf(path, sizeof path, "/?t=%s", test_token());
    r = raw_get(path, "");
    CHECK_CONTAINS(r, "302"); CHECK_CONTAINS(r, "Location: /"); CHECK_CONTAINS(r, "HttpOnly"); CHECK_CONTAINS(r, "SameSite=Strict");
    char ck[200]; snprintf(ck, sizeof ck, "Cookie: other=1; wl-%d=%s\r\n", g_port, test_token());
    free(r);
    r = raw_get("/status", ck);                                          CHECK_CONTAINS(r, "200 OK"); free(r);
    r = raw_get("/", ck);                                                CHECK_CONTAINS(r, "200 OK"); free(r);
    /* through an ssh forward the port differs from ours: fine with the secret, refused without it */
    char fwd[200]; int n = snprintf(fwd, sizeof fwd, "GET /status HTTP/1.1\r\nHost: localhost:%d\r\nX-Winless-Token: %s\r\nConnection: close\r\n\r\n", g_port + 1, test_token());
    int fd = raw_connect(); raw_send(fd, fwd, (size_t)n); size_t len; r = raw_read(fd, 5, &len); raw_close(fd);
    CHECK_CONTAINS(r, "200 OK"); free(r);
    n = snprintf(fwd, sizeof fwd, "GET /status HTTP/1.1\r\nHost: localhost:%d\r\nConnection: close\r\n\r\n", g_port + 1);
    fd = raw_connect(); raw_send(fd, fwd, (size_t)n); r = raw_read(fd, 5, &len); raw_close(fd);
    CHECK_CONTAINS(r, "403"); free(r);
    /* the secret does not make another site's page welcome */
    r = raw_request_as("POST", "localhost", "http://evil.example", cmd, test_token()); CHECK_CONTAINS(r, "403"); free(r);
    r = raw_request_as("POST", "evil.example", NULL, cmd, test_token());               CHECK_CONTAINS(r, "403"); free(r);
}

static void t_the_secret_file_is_private(void) {
    char f[1300]; winless_port_file(g_port, f, sizeof f, 0);
    struct stat st; CHECK(stat(f, &st) == 0);
    CHECK_INT(st.st_mode & 077, 0);                                      /* no access for group or others */
    char dir[1300]; snprintf(dir, sizeof dir, "%s", f); *strrchr(dir, '/') = 0;
    CHECK(stat(dir, &st) == 0);
    CHECK_INT(st.st_mode & 077, 0);
    CHECK_INT(strlen(test_token()), 64);
}

int main(void) {
    testlib_init();
    static const testcase tests[] = {
        { "ping_and_version", t_ping_and_version, 1 },
        { "viewer_page_is_served", t_viewer_page_is_served, 1 },
        { "unknown_path_and_bad_json", t_unknown_path_and_bad_json, 1 },
        { "load_and_content_roundtrip_with_hebrew_and_escapes", t_load_and_content_roundtrip_with_hebrew_and_escapes, 1 },
        { "surrogate_pair_escape_is_decoded", t_surrogate_pair_escape_is_decoded, 1 },
        { "large_text_roundtrip", t_large_text_roundtrip, 1 },
        { "commands_are_broadcast_to_the_page", t_commands_are_broadcast_to_the_page, 1 },
        { "load_broadcasts_a_bare_load_without_the_text", t_load_broadcasts_a_bare_load_without_the_text, 1 },
        { "several_pages_all_receive_commands", t_several_pages_all_receive_commands, 1 },
        { "marks_and_highlights_persist_for_reconnects", t_marks_and_highlights_persist_for_reconnects, 1 },
        { "search_and_clear_are_remembered", t_search_and_clear_are_remembered, 1 },
        { "page_state_is_readable_and_resumes", t_page_state_is_readable_and_resumes, 1 },
        { "quit_event_releases_a_waiting_request", t_quit_event_releases_a_waiting_request, 1 },
        { "only_local_pages_may_use_the_server", t_only_local_pages_may_use_the_server, 1 },
        { "the_secret_is_required", t_the_secret_is_required, 1 },
        { "the_secret_file_is_private", t_the_secret_file_is_private, 1 },
        { "wait_times_out", t_wait_times_out, 1 },
        { "closed_event_also_releases_wait", t_closed_event_also_releases_wait, 1 },
        { "disconnected_page_is_dropped_at_once", t_disconnected_page_is_dropped_at_once, 1 },
        { "garbage_and_slow_clients_do_not_hurt_the_server", t_garbage_and_slow_clients_do_not_hurt_the_server, 1 },
        { "many_connections_at_once", t_many_connections_at_once, 1 },
        { "stop_command_ends_the_server", t_stop_command_ends_the_server, 1 },
    };
    return run_tests(tests, sizeof tests / sizeof *tests) ? 1 : 0;
}
