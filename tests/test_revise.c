/* test_revise.c -- annotations handed to a model, and the review of what it proposes (with a fake model).
 * Copyright (C) 2026 David Walther.  GPL-3.0-or-later. */
#define _POSIX_C_SOURCE 200809L
#include "testlib.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- helpers ---- */
static jv *post_json(const char *path, const char *body) {
    char *r = post_ok(path, body);
    jv *j = json_parse(r, strlen(r));
    if (!j) FAIL("POST %s returned invalid JSON: %.200s", path, r);
    free(r);
    return j;
}
static void expect_ok(run_t r) { if (r.code != 0) FAIL("command failed with exit code %d: %.300s", r.code, r.out ? r.out : ""); free(r.out); }
static void wait_for_ping(void) {
    for (int i = 0; i < 100; i++) { resp_t r = http("GET", "/ping", NULL, 0); int ok = r.status == 200; free(r.body); if (ok) return; msleep(50); }
    FAIL("the server did not start");
}
static char *content_text(void) { jv *c = get_json("/content"); char *t = xstrdup(json_gets(c, "text", NULL)); json_free(c); return t; }
static int content_dirty(void) { jv *c = get_json("/content"); int d = json_getbool(c, "dirty", -1); json_free(c); return d; }
static void check_disk(const char *f, const char *want) { size_t n; char *d = read_file(f, &n); CHECK_STR(d, want); free(d); }

static char fake_dir[700];                       /* where the fake model keeps its files: REQ, REPLY, EXIT, DELAY, CWD, ARGS */
static char *fpath(const char *name) { static char p[8][800]; static int k; char *q = p[k++ % 8]; snprintf(q, 800, "%s/%s", fake_dir, name); return q; }
static void put_file(const char *name, const char *data) { FILE *f = fopen(fpath(name), "w"); if (!f) FAIL("cannot write %s", name); fputs(data, f); fclose(f); }
static void rm_file(const char *name) { unlink(fpath(name)); }
static char *slurp(const char *name) { size_t n; char *d = read_file(fpath(name), &n); return d ? d : xstrdup(""); }

static char *make_script(const char *name, const char *body) {
    char *p = tmp_write(name, "", 0);
    FILE *f = fopen(p, "w"); if (!f) FAIL("cannot write %s", name); fputs(body, f); fclose(f); chmod(p, 0755);
    return p;
}

static void start_server(void) {                  /* a server of our own, with the environment as it is now */
    g_port = free_port();
    char *serve[] = { "serve", NULL }; int out; cli_spawn(serve, &out); close(out);
    wait_for_ping();
}
static jv *review_status(void) { return post_json("/cmd", "{\"cmd\":\"review\",\"action\":\"status\"}"); }
static jv *wait_state(const char *want) {
    for (int i = 0; i < 400; i++) {
        jv *s = review_status();
        if (!strcmp(json_gets(s, "state", NULL), want)) return s;
        json_free(s); msleep(50);
    }
    FAIL("the review never reached the state %s", want);
}
static jv *review_do(const char *action, int n) {
    char b[160]; if (n) snprintf(b, sizeof b, "{\"cmd\":\"review\",\"action\":\"%s\",\"n\":%d}", action, n); else snprintf(b, sizeof b, "{\"cmd\":\"review\",\"action\":\"%s\"}", action);
    return post_json("/cmd", b);
}
static char *request_user(void) {                 /* what a custom model command received: JSON with the user message inside */
    char *raw = slurp("REQ"); jv *j = json_parse(raw, strlen(raw)); free(raw);
    if (!j) FAIL("the request the model received is not JSON");
    char *u = xstrdup(json_gets(j, "user", NULL)); json_free(j); return u;
}
static int count_in(const char *hay, const char *needle) { int n = 0; for (const char *p = hay; (p = strstr(p, needle)); p++) n++; return n; }

/* The document every test marks up.  Line 1: "very good" (col 5-13); line 3 is kept; line 4: "quite poor" (col 11-20). */
#define DOC "The very good day began.\n\nSecond line is fine.\nThird has quite poor words.\n"
static char *open_doc(const char *name, const char *text) {
    char *f = tmp_write(name, text, strlen(text));
    expect_ok(CLI("open", f, "--no-window"));
    return f;
}
static void mark_doc(void) {
    expect_ok(CLI("annotate", "--kind", "slop", "1", "5", "1", "14"));
    expect_ok(CLI("annotate", "--kind", "keep", "3", "1", "3", "21"));
    expect_ok(CLI("annotate", "--kind", "note", "4", "11", "4", "21", "cut this"));
}
static const char *EDITS_MIXED =
    "{\"edits\":[{\"annotation\":%ld,\"find\":\"very good\",\"replace\":\"good\",\"why\":\"filler\"},"
    "{\"annotation\":%ld,\"find\":\"quite poor\",\"replace\":\"poor\",\"why\":\"tighter\"},"
    "{\"annotation\":%ld,\"also\":true,\"find\":\"is fine\",\"replace\":\"is great\",\"why\":\"touches a keep\"},"
    "{\"annotation\":%ld,\"find\":\"not in the text\",\"replace\":\"x\"}]}";

static void ids_of_marks(long *slop, long *keep, long *note) {
    jv *l = post_json("/cmd", "{\"cmd\":\"annotations\"}");
    const jv *items = json_get(l, "items");
    for (size_t i = 0; i < items->n; i++) {
        const char *k = json_gets(items->items[i], "kind", NULL); long id = (long)json_getnum(items->items[i], "id", 0);
        if (!strcmp(k, "slop")) *slop = id; else if (!strcmp(k, "keep")) *keep = id; else *note = id;
    }
    json_free(l);
}
static void set_reply_mixed(void) {
    long s, k, n; ids_of_marks(&s, &k, &n);
    char buf[1200]; snprintf(buf, sizeof buf, EDITS_MIXED, s, n, s, n);
    put_file("REPLY", buf);
}
static const char *status_of(const jv *l, long id) {
    const jv *items = json_get(l, "items");
    for (size_t i = 0; i < items->n; i++) if ((long)json_getnum(items->items[i], "id", 0) == id) return json_gets(items->items[i], "status", NULL);
    return "?";
}

/* ---- tests ---- */
static void t_accept_some_reject_some_and_finish(void) {
    start_server();
    char *f = open_doc("rv1.txt", DOC);
    mark_doc(); set_reply_mixed();
    run_t r = CLI("revise", "--wait"); CHECK_INT(r.code, 0); CHECK_CONTAINS(r.out, "ready"); free(r.out);

    char *req = request_user();                                                  /* what the model was told */
    CHECK_CONTAINS(req, "<document>"); CHECK_CONTAINS(req, "Second line is fine."); CHECK_CONTAINS(req, "\"text\":\"very good\"");
    CHECK_CONTAINS(req, "\"kind\":\"slop\""); CHECK_CONTAINS(req, "\"kind\":\"keep\""); CHECK_CONTAINS(req, "cut this");
    free(req);

    jv *st = review_status();
    CHECK_STR(json_gets(st, "state", NULL), "ready"); CHECK_INT(json_get(st, "islands")->n, 2); CHECK_INT(json_get(st, "problems")->n, 2);
    CHECK_INT(json_getnum(st, "notes", 0), 2);                                  /* the keep is protection, not a note to revise */
    const jv *p0 = json_get(st, "problems")->items[0], *p1 = json_get(st, "problems")->items[1];
    CHECK(strstr(json_gets(p0, "problem", NULL), "keep") || strstr(json_gets(p1, "problem", NULL), "keep"));        /* the edit touching the keep was refused */
    CHECK(strstr(json_gets(p0, "problem", NULL), "not found") || strstr(json_gets(p1, "problem", NULL), "not found"));
    json_free(st);
    r = CLI("review", "status"); CHECK_CONTAINS(r.out, "[pending]"); CHECK_CONTAINS(r.out, "could not apply"); free(r.out);

    jv *a = review_do("accept", 1); CHECK_INT(json_getbool(a, "ok", 0), 1); json_free(a);
    a = review_do("reject", 2); json_free(a);
    st = review_status(); CHECK_INT(json_getnum(st, "accepted", 0), 1); CHECK_INT(json_getnum(st, "rejected", 0), 1); CHECK_INT(json_getnum(st, "pending", 0), 0); json_free(st);
    char *t = content_text(); CHECK_STR(t, DOC); free(t);                      /* nothing is applied before finishing */

    jv *fin = review_do("finish", 0);
    CHECK_INT(json_getbool(fin, "ok", 0), 1); CHECK_INT(json_getbool(fin, "changed", 0), 1); CHECK_INT(json_getbool(fin, "written", 0), 1);
    CHECK_CONTAINS(json_gets(fin, "note", NULL), "not committed"); json_free(fin);                                         /* a plain file is not in a repository */
    t = content_text(); CHECK_STR(t, "The good day began.\n\nSecond line is fine.\nThird has quite poor words.\n"); free(t);
    check_disk(f, "The good day began.\n\nSecond line is fine.\nThird has quite poor words.\n");
    CHECK_INT(content_dirty(), 0);

    long s, k, n; ids_of_marks(&s, &k, &n);
    jv *l = post_json("/cmd", "{\"cmd\":\"annotations\"}");
    CHECK_STR(status_of(l, s), "done"); CHECK_STR(status_of(l, n), "open"); CHECK_STR(status_of(l, k), "open");          /* only the change that was accepted finishes its note */
    json_free(l);
    jv *h = post_json("/cmd", "{\"cmd\":\"history\"}"); CHECK_INT(json_get(h, "items")->n, 1); json_free(h);          /* the write kept the old file */
    st = review_status(); CHECK_STR(json_gets(st, "state", NULL), "none"); json_free(st);

    expect_ok(CLI("undo"));                                                                                          /* the whole review is ONE undo step */
    t = content_text(); CHECK_STR(t, DOC); free(t);
}

static void git_in(const char *dir, const char *cmd) {
    char c[2000]; snprintf(c, sizeof c, "cd '%s' && %s", dir, cmd);
    if (system(c) != 0) FAIL("git setup failed: %s", cmd);
}
static char *git_out(const char *dir, const char *cmd) {
    char c[2000]; snprintf(c, sizeof c, "cd '%s' && %s", dir, cmd);
    FILE *p = popen(c, "r"); static char buf[2000]; size_t n = p ? fread(buf, 1, sizeof buf - 1, p) : 0; if (p) pclose(p); buf[n] = 0; return buf;
}

static void t_commit_only_when_the_file_was_clean(void) {
    setenv("GIT_AUTHOR_NAME", "Test", 1); setenv("GIT_AUTHOR_EMAIL", "t@example.com", 1); setenv("GIT_COMMITTER_NAME", "Test", 1); setenv("GIT_COMMITTER_EMAIL", "t@example.com", 1);
    char repo[800]; snprintf(repo, sizeof repo, "%s/repo1", tmp_dir()); mkdir(repo, 0755);
    char path[900]; snprintf(path, sizeof path, "%s/book.md", repo);
    { FILE *f = fopen(path, "w"); fputs(DOC, f); fclose(f); }
    git_in(repo, "git init -q && git add book.md && git commit -q -m first");
    start_server();
    expect_ok(CLI("open", path, "--no-window"));
    mark_doc(); set_reply_mixed();
    expect_ok(CLI("revise", "--wait"));
    jv *a = review_do("accept", 1); json_free(a);
    jv *fin = review_do("finish", 0);
    CHECK_INT(json_getbool(fin, "written", 0), 1); CHECK(json_get(fin, "commit")->t == J_STR); CHECK_CONTAINS(json_gets(fin, "note", NULL), "committed"); json_free(fin);
    CHECK_STR(git_out(repo, "git log --format=%s | head -1"), "winless: revision of book.md (1 of 2 changes accepted)\n");
    CHECK_STR(git_out(repo, "git log --oneline | wc -l | tr -d ' '"), "2\n");
    CHECK_STR(git_out(repo, "git status --porcelain"), "");                                                           /* nothing left over */
}

static void t_no_commit_when_the_file_had_uncommitted_changes(void) {
    setenv("GIT_AUTHOR_NAME", "Test", 1); setenv("GIT_AUTHOR_EMAIL", "t@example.com", 1); setenv("GIT_COMMITTER_NAME", "Test", 1); setenv("GIT_COMMITTER_EMAIL", "t@example.com", 1);
    char repo[800]; snprintf(repo, sizeof repo, "%s/repo2", tmp_dir()); mkdir(repo, 0755);
    char path[900]; snprintf(path, sizeof path, "%s/book.md", repo);
    { FILE *f = fopen(path, "w"); fputs("An older line.\n", f); fclose(f); }
    git_in(repo, "git init -q && git add book.md && git commit -q -m first");
    { FILE *f = fopen(path, "w"); fputs(DOC, f); fclose(f); }                                                         /* the author's own uncommitted edit */
    start_server();
    expect_ok(CLI("open", path, "--no-window"));
    mark_doc(); set_reply_mixed();
    expect_ok(CLI("revise", "--wait"));
    jv *a = review_do("accept", 1); json_free(a);
    jv *fin = review_do("finish", 0);
    CHECK_INT(json_getbool(fin, "written", 0), 1); CHECK(json_get(fin, "commit")->t == J_NULL); CHECK_CONTAINS(json_gets(fin, "note", NULL), "not committed"); json_free(fin);
    CHECK_STR(git_out(repo, "git log --oneline | wc -l | tr -d ' '"), "1\n");                                         /* the author's edits were not swept into a commit */
    CHECK_CONTAINS(git_out(repo, "git status --porcelain"), "book.md");
}

static void t_bad_replies_and_failing_models(void) {
    start_server();
    open_doc("rv3.txt", DOC); mark_doc();
    put_file("REPLY", "I am sorry, I could not do that.");                                     /* not JSON */
    run_t r = CLI("revise", "--wait"); CHECK_INT(r.code, 1); free(r.out);
    jv *st = review_status(); CHECK_STR(json_gets(st, "state", NULL), "error"); CHECK_CONTAINS(json_gets(st, "error", NULL), "could not be used");
    CHECK_CONTAINS(json_gets(st, "raw", NULL), "I am sorry"); json_free(st);

    put_file("REPLY", "{\"edits\":[{\"annotation\":1,\"find\":\"nowhere in the text\",\"replace\":\"x\"}]}");     /* every edit unusable: ready, with nothing to review */
    r = CLI("revise", "--wait"); CHECK_INT(r.code, 0); free(r.out);
    st = review_status(); CHECK_INT(json_get(st, "islands")->n, 0); CHECK_INT(json_get(st, "problems")->n, 1); json_free(st);
    jv *fin = review_do("finish", 0); CHECK_CONTAINS(json_gets(fin, "note", NULL), "nothing was accepted"); CHECK_INT(json_getbool(fin, "changed", 1), 0); json_free(fin);
    char *t = content_text(); CHECK_STR(t, DOC); free(t);

    put_file("REPLY", "{\"edits\":[]}"); put_file("EXIT", "3");                                  /* the model command fails */
    r = CLI("revise", "--wait"); CHECK_INT(r.code, 1); free(r.out);
    st = review_status(); CHECK_STR(json_gets(st, "state", NULL), "error");
    CHECK_CONTAINS(json_gets(st, "error", NULL), "exit 3"); CHECK_CONTAINS(json_gets(st, "error", NULL), "model went wrong"); json_free(st);
    rm_file("EXIT");

    put_file("REPLY", "{\"edits\":[]}");                                                          /* a model with nothing to propose is a normal answer */
    r = CLI("revise", "--wait"); CHECK_INT(r.code, 0); free(r.out);
    st = review_status(); CHECK_STR(json_gets(st, "state", NULL), "ready"); CHECK_INT(json_get(st, "islands")->n, 0); json_free(st);
    json_free(review_do("cancel", 0));
}

static void t_cancel_stops_the_model(void) {
    start_server();
    open_doc("rv4.txt", DOC); mark_doc(); put_file("REPLY", "{\"edits\":[]}"); put_file("DELAY", "30");
    expect_ok(CLI("revise"));
    jv *st = wait_state("running"); json_free(st);
    jv *again = post_json("/cmd", "{\"cmd\":\"revise\"}"); CHECK_INT(json_getbool(again, "ok", 1), 0); json_free(again);     /* one at a time */
    json_free(review_do("cancel", 0));
    st = review_status(); CHECK_STR(json_gets(st, "state", NULL), "none"); json_free(st);
    jv *p = get_json("/ping"); json_free(p);                                                      /* and the server is still there */
    rm_file("DELAY");
}

static void t_a_slow_model_times_out(void) {
    setenv("WINLESS_LLM_TIMEOUT", "1", 1);
    start_server();
    unsetenv("WINLESS_LLM_TIMEOUT");
    open_doc("rv5.txt", DOC); mark_doc(); put_file("REPLY", "{\"edits\":[]}"); put_file("DELAY", "30");
    expect_ok(CLI("revise"));
    jv *st = wait_state("error"); CHECK_CONTAINS(json_gets(st, "error", NULL), "in time"); json_free(st);
    rm_file("DELAY");
}

static void t_scopes_choose_the_notes_sent(void) {
    start_server();
    open_doc("rv6.txt", DOC); mark_doc(); put_file("REPLY", "{\"edits\":[]}");
    long s, k, n; ids_of_marks(&s, &k, &n);
    expect_ok(CLI("revise", "--wait"));                                                          /* all open notes */
    char *req = request_user(); CHECK_CONTAINS(req, "\"kind\":\"slop\""); CHECK_CONTAINS(req, "\"kind\":\"note\""); CHECK_CONTAINS(req, "\"kind\":\"keep\""); free(req);
    json_free(review_do("cancel", 0));
    char ids[16]; snprintf(ids, sizeof ids, "%ld", n);
    expect_ok(CLI("revise", "--id", ids, "--wait"));                                             /* one note: the keep still goes along, the slop does not */
    req = request_user(); CHECK_CONTAINS(req, "\"kind\":\"note\""); CHECK_CONTAINS(req, "\"kind\":\"keep\""); CHECK_INT(count_in(req, "\"kind\":\"slop\""), 0); free(req);
    json_free(review_do("cancel", 0));
    expect_ok(CLI("revise", "--range", "1", "1", "2", "1", "--wait"));                           /* the notes inside lines 1-2 */
    req = request_user(); CHECK_CONTAINS(req, "\"kind\":\"slop\""); CHECK_INT(count_in(req, "\"kind\":\"note\""), 0); free(req);
    json_free(review_do("cancel", 0));
}

static void t_no_notes_means_no_request(void) {
    start_server();
    rm_file("REQ");
    open_doc("rv7.txt", DOC);
    jv *r = post_json("/cmd", "{\"cmd\":\"revise\"}");
    CHECK_INT(json_getbool(r, "ok", 1), 0); CHECK_CONTAINS(json_gets(r, "error", NULL), "no open notes"); json_free(r);
    expect_ok(CLI("annotate", "--kind", "keep", "1", "1", "1", "4"));                           /* a keep alone is not something to revise */
    r = post_json("/cmd", "{\"cmd\":\"revise\"}"); CHECK_INT(json_getbool(r, "ok", 1), 0); json_free(r);
    char *req = slurp("REQ"); CHECK_STR(req, ""); free(req);                                      /* the model was never called */
}

static void t_the_claude_backend_command_line(void) {
    char *script = make_script("fake-claude.sh",
        "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$FAKE_DIR/ARGS\"\npwd > \"$FAKE_DIR/CWD\"\ncat > \"$FAKE_DIR/REQ\"\ncat \"$FAKE_DIR/REPLY\"\n");
    char *safe = getenv("WINLESS_CLAUDE"); char *keep_safe = safe ? xstrdup(safe) : NULL;
    char *llm = getenv("WINLESS_LLM"); char *keep_llm = llm ? xstrdup(llm) : NULL;
    setenv("WINLESS_LLM", "", 1); setenv("WINLESS_CLAUDE", script, 1);       /* an empty $WINLESS_LLM means: use the claude backend */
    start_server();
    open_doc("rv8.txt", DOC); mark_doc(); set_reply_mixed();
    expect_ok(CLI("revise", "--wait"));
    char *args = slurp("ARGS");
    CHECK_CONTAINS(args, "-p\n"); CHECK_CONTAINS(args, "--tools\n\n"); CHECK_CONTAINS(args, "--no-session-persistence\n"); CHECK_CONTAINS(args, "--system-prompt\n");
    CHECK_CONTAINS(args, "editing assistant");                                                    /* the fixed instructions travel as the system prompt */
    free(args);
    char *req = slurp("REQ"); CHECK_CONTAINS(req, "<document>"); CHECK_CONTAINS(req, "very good"); free(req);       /* the document and notes arrive on stdin */
    char *cwd = slurp("CWD"); CHECK_CONTAINS(cwd, "winless"); CHECK(!strstr(cwd, "rv8")); free(cwd);                /* run in winless's own directory, not the book's */
    jv *st = review_status(); CHECK_STR(json_gets(st, "backend", NULL), "claude"); CHECK_INT(json_get(st, "islands")->n, 2); json_free(st);
    json_free(review_do("cancel", 0));
    jv *r = post_json("/cmd", "{\"cmd\":\"backend\",\"value\":\"api\"}"); CHECK_INT(json_getbool(r, "ok", 0), 1); json_free(r);
    r = post_json("/cmd", "{\"cmd\":\"revise\"}"); CHECK_CONTAINS(json_gets(r, "error", NULL), "not built yet"); json_free(r);   /* the api backend comes later */
    r = post_json("/cmd", "{\"cmd\":\"backend\",\"value\":\"nonsense\"}"); CHECK_INT(json_getbool(r, "ok", 1), 0); json_free(r);
    r = post_json("/cmd", "{\"cmd\":\"backend\",\"value\":\"claude\"}"); json_free(r);
    expect_ok(CLI("revise", "--wait"));                                                          /* and back */
    json_free(review_do("cancel", 0));
    if (keep_llm) setenv("WINLESS_LLM", keep_llm, 1);
    if (keep_safe) setenv("WINLESS_CLAUDE", keep_safe, 1);
    free(keep_llm); free(keep_safe);
}

static void t_unsaved_edits_are_not_written(void) {
    start_server();
    char *f = open_doc("rv9.txt", DOC); mark_doc(); set_reply_mixed();
    expect_ok(CLI("edit", "2", "1", "2", "1", "extra"));                                         /* the author's own unsaved edit, elsewhere */
    expect_ok(CLI("revise", "--wait"));
    json_free(review_do("accept-all", 0));
    jv *fin = review_do("finish", 0);
    CHECK_INT(json_getbool(fin, "changed", 0), 1); CHECK_INT(json_getbool(fin, "written", 1), 0); CHECK_CONTAINS(json_gets(fin, "note", NULL), "unsaved"); json_free(fin);
    char *t = content_text(); CHECK_STR(t, "The good day began.\nextra\nSecond line is fine.\nThird has poor words.\n"); free(t);
    check_disk(f, DOC);                                                                          /* the file on disk was left alone */
    CHECK_INT(content_dirty(), 1);
}

static void t_text_that_changed_while_the_model_worked(void) {
    start_server();
    open_doc("rv10.txt", DOC); mark_doc(); set_reply_mixed(); put_file("DELAY", "1");
    expect_ok(CLI("revise"));
    expect_ok(CLI("edit", "2", "1", "2", "1", "typed meanwhile"));
    jv *st = wait_state("ready"); json_free(st);
    json_free(review_do("accept-all", 0));
    jv *fin = review_do("finish", 0);
    CHECK_INT(json_getbool(fin, "ok", 1), 0); CHECK_CONTAINS(json_gets(fin, "error", NULL), "changed while the model"); json_free(fin);   /* refused, nothing lost */
    char *t = content_text(); CHECK_STR(t, "The very good day began.\ntyped meanwhile\nSecond line is fine.\nThird has quite poor words.\n"); free(t);
    json_free(review_do("cancel", 0)); rm_file("DELAY");
}

static void t_the_author_can_use_their_own_wording(void) {
    start_server();
    open_doc("rv11.txt", DOC); mark_doc(); set_reply_mixed();
    expect_ok(CLI("revise", "--wait"));
    expect_ok(CLI("review", "edit", "1", "My own first line."));
    jv *st = review_status(); const jv *i1 = json_get(st, "islands")->items[0];
    CHECK_STR(json_gets(i1, "new", NULL), "My own first line."); CHECK_INT(json_getbool(i1, "custom", 0), 1); json_free(st);
    expect_ok(CLI("review", "accept", "1"));
    run_t r = CLI("review", "finish"); CHECK_INT(r.code, 0); CHECK_CONTAINS(r.out, "accepted 1"); free(r.out);
    char *t = content_text(); CHECK_STR(t, "My own first line.\n\nSecond line is fine.\nThird has quite poor words.\n"); free(t);
    run_t bad = CLI("review", "accept", "9"); CHECK_INT(bad.code, 1); free(bad.out);
}

int main(void) {
    testlib_init();
    snprintf(fake_dir, sizeof fake_dir, "%s/fake", tmp_dir()); mkdir(fake_dir, 0755);
    setenv("FAKE_DIR", fake_dir, 1);
    char *model = make_script("fake-model.sh",
        "#!/bin/sh\n"
        "cat > \"$FAKE_DIR/REQ\"\n"
        "[ -f \"$FAKE_DIR/DELAY\" ] && sleep \"$(cat \"$FAKE_DIR/DELAY\")\"\n"
        "if [ -f \"$FAKE_DIR/EXIT\" ]; then echo 'the model went wrong' >&2; exit \"$(cat \"$FAKE_DIR/EXIT\")\"; fi\n"
        "cat \"$FAKE_DIR/REPLY\"\n");
    char cmd[900]; snprintf(cmd, sizeof cmd, "sh %s", model);
    setenv("WINLESS_LLM", cmd, 1);                                           /* every test's server asks this fake model ... */
    setenv("WINLESS_CLAUDE", make_script("safe-claude.sh", "#!/bin/sh\ncat > /dev/null\necho '{\"edits\":[]}'\n"), 1);   /* ... and the real claude can never be run by mistake */
    static const testcase tests[] = {
        { "accept_some_reject_some_and_finish", t_accept_some_reject_some_and_finish, 0 },
        { "commit_only_when_the_file_was_clean", t_commit_only_when_the_file_was_clean, 0 },
        { "no_commit_when_the_file_had_uncommitted_changes", t_no_commit_when_the_file_had_uncommitted_changes, 0 },
        { "bad_replies_and_failing_models", t_bad_replies_and_failing_models, 0 },
        { "cancel_stops_the_model", t_cancel_stops_the_model, 0 },
        { "a_slow_model_times_out", t_a_slow_model_times_out, 0 },
        { "scopes_choose_the_notes_sent", t_scopes_choose_the_notes_sent, 0 },
        { "no_notes_means_no_request", t_no_notes_means_no_request, 0 },
        { "unsaved_edits_are_not_written", t_unsaved_edits_are_not_written, 0 },
        { "text_that_changed_while_the_model_worked", t_text_that_changed_while_the_model_worked, 0 },
        { "the_author_can_use_their_own_wording", t_the_author_can_use_their_own_wording, 0 },
        { "the_claude_backend_command_line", t_the_claude_backend_command_line, 0 },
    };
    return run_tests(tests, sizeof tests / sizeof *tests) ? 1 : 0;
}
