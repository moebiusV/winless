/* test_source.c -- a source (winless_source.h) answering a window's hook: a fake source library is compiled by the test
 * and loaded by the real server.  Copyright (C) 2026 David Walther.  GPL-3.0-or-later. */
#define _POSIX_C_SOURCE 200809L
#include "testlib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char FAKE_SOURCE[] =
    "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
    "#include \"winless_source.h\"\n"
    "static void *op(const char *spec, char **err) {\n"
    "    if (!strcmp(spec, \"refuse\")) { *err = strdup(\"fake: refused to open\"); return NULL; }\n"
    "    return strdup(spec);\n"
    "}\n"
    "static int fetch(void *h, const char *arg, const char *text, char **out, char **err) {\n"
    "    if (!strcmp(arg, \"boom\")) { *err = strdup(\"fake: boom\"); return -1; }\n"
    "    char *b = malloc(strlen(arg) + strlen(text) + strlen((char *)h) + 64);\n"
    "    sprintf(b, \"spec=%s arg=%s textlen=%zu\\n\", (char *)h, arg, strlen(text));\n"
    "    *out = b; return 0;\n"
    "}\n"
    "static void cl(void *h) { free(h); }\n"
    "static const wl_source S = { WL_SOURCE_ABI, \"fake\", op, fetch, cl };\n"
    "const wl_source *wl_source_entry(void) { return &S; }\n";

static jv *post_json(const char *path, const char *body) {
    char *r = post_ok(path, body);
    jv *j = json_parse(r, strlen(r));
    if (!j) FAIL("POST %s returned invalid JSON: %.200s", path, r);
    free(r);
    return j;
}

static void t_a_source_answers_the_hook_without_a_shell(void) {
    jv *r = post_json("/cmd", "{\"cmd\":\"load\",\"name\":\"list\",\"text\":\"* a #7\\n\",\"settings\":{\"outline\":true,\"source\":\"fake:the-data\"}}");
    CHECK_INT(json_getbool(r, "ok", 0), 1);
    json_free(r);
    jv *c = get_json("/content");                                  /* the page learns that Enter can ask */
    CHECK(json_gets(json_get(c, "settings"), "hook", NULL) != NULL);
    json_free(c);
    r = post_json("/cmd", "{\"cmd\":\"hook\",\"arg\":\"thread:42\"}");
    CHECK_INT(json_getbool(r, "ok", 0), 1);
    CHECK_STR(json_gets(r, "text", NULL), "spec=the-data arg=thread:42 textlen=0\n");
    json_free(r);
    /* what the shell hook refuses is fine for a source: an argument is data, not a command line */
    r = post_json("/cmd", "{\"cmd\":\"hook\",\"arg\":\"stream:speaker:Ann Lee;rm -rf $HOME\",\"text\":\"~u cheese | ~k x\"}");
    CHECK_INT(json_getbool(r, "ok", 0), 1);
    CHECK_STR(json_gets(r, "text", NULL), "spec=the-data arg=stream:speaker:Ann Lee;rm -rf $HOME textlen=16\n");
    json_free(r);
    /* free text is not cut at 1024 bytes as the shell hook's $WINLESS_TEXT is */
    char big[3000]; memset(big, 'x', sizeof big); big[sizeof big - 1] = 0;
    char body[3300]; snprintf(body, sizeof body, "{\"cmd\":\"hook\",\"arg\":\"list:limit\",\"text\":\"%s\"}", big);
    r = post_json("/cmd", body);
    CHECK_STR(json_gets(r, "text", NULL), "spec=the-data arg=list:limit textlen=2999\n");
    json_free(r);
}

static void t_a_failing_source_is_reported_and_bad_arguments_refused(void) {
    jv *r = post_json("/cmd", "{\"cmd\":\"load\",\"name\":\"list\",\"text\":\"x\\n\",\"settings\":{\"source\":\"fake:data\"}}");
    json_free(r);
    r = post_json("/cmd", "{\"cmd\":\"hook\",\"arg\":\"boom\"}");
    CHECK_INT(json_getbool(r, "ok", 1), 0); CHECK_STR(json_gets(r, "error", NULL), "fake: boom");
    json_free(r);
    r = post_json("/cmd", "{\"cmd\":\"hook\",\"arg\":\"\"}");
    CHECK_INT(json_getbool(r, "ok", 1), 0); CHECK_CONTAINS(json_gets(r, "error", NULL), "bad argument");
    json_free(r);
    r = post_json("/cmd", "{\"cmd\":\"hook\"}");
    CHECK_INT(json_getbool(r, "ok", 1), 0);
    json_free(r);
}

static void t_a_bad_source_refuses_the_load_and_keeps_the_window(void) {
    jv *r = post_json("/cmd", "{\"cmd\":\"load\",\"name\":\"first\",\"text\":\"keep me\\n\"}");
    json_free(r);
    const char *bad[] = {
        "{\"cmd\":\"load\",\"name\":\"x\",\"text\":\"new\\n\",\"settings\":{\"source\":\"nothere:data\"}}",
        "{\"cmd\":\"load\",\"name\":\"x\",\"text\":\"new\\n\",\"settings\":{\"source\":\"no-colon\"}}",
        "{\"cmd\":\"load\",\"name\":\"x\",\"text\":\"new\\n\",\"settings\":{\"source\":\"../etc:data\"}}",
        "{\"cmd\":\"load\",\"name\":\"x\",\"text\":\"new\\n\",\"settings\":{\"source\":\"fake:refuse\"}}",
    };
    const char *want[] = { "no source", "NAME:SPEC", "bad source name", "refused to open" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        r = post_json("/cmd", bad[i]);
        CHECK_INT(json_getbool(r, "ok", 1), 0); CHECK_CONTAINS(json_gets(r, "error", NULL), want[i]);
        json_free(r);
    }
    jv *c = get_json("/content");                                  /* the refused loads changed nothing */
    CHECK_STR(json_gets(c, "name", NULL), "first");
    CHECK_CONTAINS(json_gets(c, "text", NULL), "keep me");
    json_free(c);
}

int main(void) {
    testlib_init();
    char dir[800], src[900], cmd[2200];
    snprintf(dir, sizeof dir, "%s/sources", tmp_dir());
    mkdir(dir, 0755);
    snprintf(src, sizeof src, "%s/fake.c", dir);
    FILE *f = fopen(src, "w"); if (!f) FAIL("cannot write %s", src);
    fputs(FAKE_SOURCE, f); fclose(f);
    snprintf(cmd, sizeof cmd, "cc -shared -fPIC -I%s -o %s/fake.so %s 2>&1", getenv("WINLESS_SRCDIR") ? getenv("WINLESS_SRCDIR") : ".", dir, src);
    if (system(cmd) != 0) { fprintf(stderr, "tests: cannot build the fake source library (no C compiler?)\n"); return 77; }
    setenv("WINLESS_SOURCE_DIR", dir, 1);                          /* the servers the tests start inherit this */

    static const testcase tests[] = {
        { "a_source_answers_the_hook_without_a_shell", t_a_source_answers_the_hook_without_a_shell, 1 },
        { "a_failing_source_is_reported_and_bad_arguments_refused", t_a_failing_source_is_reported_and_bad_arguments_refused, 1 },
        { "a_bad_source_refuses_the_load_and_keeps_the_window", t_a_bad_source_refuses_the_load_and_keeps_the_window, 1 },
    };
    return run_tests(tests, sizeof tests / sizeof *tests) ? 1 : 0;
}
