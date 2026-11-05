/* test_unit.c -- unit tests for util.c (UTF-8 cleaning, buffers) and json.c.  Copyright (C) 2026 David Walther. GPL-3.0-or-later. */
#define _POSIX_C_SOURCE 200809L
#include "testlib.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static void check_clean(const char *in, size_t n, const char *want) {
    sb_t b; sb_init(&b);
    sb_put_utf8_clean(&b, in, n);
    CHECK_STR(b.s, want);
    sb_free(&b);
}
#define CLEAN(in, want) check_clean((in), sizeof(in) - 1, (want))
#define FFFD "\xEF\xBF\xBD"

static void t_sb_basics(void) {
    sb_t b; sb_init(&b);
    sb_puts(&b, "ab"); sb_putc(&b, 'c'); sb_printf(&b, "-%d-%s", 42, "x"); sb_put(&b, "yz", 1);
    CHECK_STR(b.s, "abc-42-xy");
    CHECK_INT(b.len, 9);
    for (int i = 0; i < 5000; i++) sb_putc(&b, 'q');               /* forces several reallocations */
    CHECK_INT(b.len, 5009);
    size_t n; char *d = sb_detach(&b, &n);
    CHECK_INT(n, 5009); CHECK(d[5009] == 0);
    free(d);
}

static void t_utf8_valid_is_kept(void) {
    CLEAN("plain ascii", "plain ascii");
    CLEAN("caf\xC3\xA9", "caf\xC3\xA9");                            /* 2 bytes */
    CLEAN("\xE2\x82\xAC", "\xE2\x82\xAC");                          /* 3 bytes: euro */
    CLEAN("\xF0\x9F\x98\x80", "\xF0\x9F\x98\x80");                  /* 4 bytes: emoji */
    CLEAN("\xD7\x90\xD6\xB8", "\xD7\x90\xD6\xB8");                  /* Hebrew aleph + qamats */
    CLEAN("\xEF\xBF\xBD", "\xEF\xBF\xBD");                          /* U+FFFD itself */
    CLEAN("\xF4\x8F\xBF\xBF", "\xF4\x8F\xBF\xBF");                  /* U+10FFFF, the last code point */
}

static void t_utf8_invalid_becomes_fffd(void) {
    CLEAN("a\xFF" "b", "a" FFFD "b");                               /* never valid */
    CLEAN("\x80", FFFD);                                            /* lone continuation byte */
    CLEAN("\xC0\x80", FFFD FFFD);                                   /* overlong NUL */
    CLEAN("\xC1\xBF", FFFD FFFD);                                   /* overlong */
    CLEAN("\xE0\x80\x80", FFFD FFFD FFFD);                          /* overlong 3-byte */
    CLEAN("\xED\xA0\x80", FFFD FFFD FFFD);                          /* UTF-16 surrogate */
    CLEAN("\xF4\x90\x80\x80", FFFD FFFD FFFD FFFD);                 /* above U+10FFFF */
    CLEAN("\xF8\x88\x80\x80\x80", FFFD FFFD FFFD FFFD FFFD);        /* 5-byte form */
    CLEAN("ok \xE2\x82 cut", "ok " FFFD FFFD " cut");               /* truncated 3-byte sequence */
    CLEAN("end\xE2\x82", "end" FFFD FFFD);                          /* truncated at the very end */
    CLEAN("end\xF0", "end" FFFD);
    CLEAN("\xF0\x9F\x98", FFFD FFFD FFFD);
}

static void t_utf8_nul_becomes_fffd(void) {
    check_clean("a\0b", 3, "a" FFFD "b");
    check_clean("", 0, "");
}

static void t_json_parses_structures(void) {
    const char *src = " { \"a\" : 1 , \"b\" : [ true , false , null , \"x\" ] , \"c\" : { \"d\" : \"e\" } } ";
    jv *v = json_parse(src, strlen(src));
    CHECK(v && v->t == J_OBJ && v->n == 3);
    CHECK_INT(json_getnum(v, "a", -1), 1);
    const jv *b = json_get(v, "b");
    CHECK(b && b->t == J_ARR && b->n == 4);
    CHECK(b->items[0]->t == J_BOOL && b->items[0]->b == 1 && b->items[1]->b == 0 && b->items[2]->t == J_NULL);
    CHECK_STR(b->items[3]->str, "x");
    CHECK_STR(json_gets(json_get(v, "c"), "d", NULL), "e");
    CHECK(json_get(v, "missing") == NULL);
    json_free(v);
}

static void t_json_string_escapes(void) {
    const char *src = "\"q\\\" b\\\\ s\\/ n\\n t\\t r\\r f\\f bs\\b u\\u00e9 \\u20ac \\ud83d\\ude00\"";
    jv *v = json_parse(src, strlen(src));
    CHECK(v && v->t == J_STR);
    CHECK_STR(v->str, "q\" b\\ s/ n\n t\t r\r f\f bs\b u\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80");
    json_free(v);
    jv *z = json_parse("\"a\\u0000b\"", 10);                      /* an embedded NUL keeps its length */
    CHECK(z && z->slen == 3 && z->str[1] == 0 && z->str[2] == 'b');
    json_free(z);
    jv *h = json_parse("\"\xD7\x90\xD6\xB8 raw utf-8 passes through\"", 28 + 0);
    CHECK(h == NULL || h->t == J_STR);                              /* length argument deliberately short: must not crash */
    json_free(h);
}

static void t_json_numbers(void) {
    const char *src = "[0, -0, 3, -1.5e2, 2E3, 0.25, 12345678901]";
    jv *v = json_parse(src, strlen(src));
    CHECK(v && v->t == J_ARR && v->n == 7);
    CHECK(v->items[0]->num == 0 && v->items[2]->num == 3 && v->items[3]->num == -150 && v->items[4]->num == 2000);
    CHECK(fabs(v->items[5]->num - 0.25) < 1e-12 && v->items[6]->num == 12345678901.0);
    json_free(v);
}

static void t_json_rejects_bad_input(void) {
    const char *bad[] = { "", "{", "[1,]", "{\"a\":1,}", "{\"a\"}", "{a:1}", "\"unterminated", "\"bad\\q\"", "\"bad\\u12\"",
                          "\"\\ud83d\"", "\"\\ude00\"", "[1] 2", "tru", "nul", "-", "[1 2]", "{\"a\":1 \"b\":2}", "'single'", "\"ctl\x01\"" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        jv *v = json_parse(bad[i], strlen(bad[i]));
        if (v) FAIL("accepted invalid JSON: [%s]", bad[i]);
    }
}

static void t_json_depth_limit(void) {
    char deep[200], ok[200];
    memset(deep, '[', 70); memset(deep + 70, ']', 70); deep[140] = 0;
    memset(ok, '[', 50); memset(ok + 50, ']', 50); ok[100] = 0;
    CHECK(json_parse(deep, 140) == NULL);
    jv *v = json_parse(ok, 100);
    CHECK(v != NULL);
    json_free(v);
}

static void t_json_write_is_compact_and_roundtrips(void) {
    const char *src = "{\"a\":1,\"b\":[true,false,null,\"x\\ny\"],\"c\":{\"d\":0.5,\"e\":-7}}";
    jv *v = json_parse(src, strlen(src));
    CHECK(v != NULL);
    sb_t b; sb_init(&b);
    json_write(&b, v);
    CHECK_STR(b.s, src);
    sb_free(&b); json_free(v);
}

static void t_json_put_string_and_number(void) {
    sb_t b; sb_init(&b);
    const char *in = "q\" b\\ \n\t\r \x01 \x1f \xC3\xA9";
    json_put_string(&b, in, strlen(in));
    CHECK_STR(b.s, "\"q\\\" b\\\\ \\n\\t\\r \\u0001 \\u001f \xC3\xA9\"");
    sb_free(&b);
    double vals[] = { 0, 5, -5, 1e15, 0.5, 123456789012.0, NAN, INFINITY };
    const char *want[] = { "0", "5", "-5", "1000000000000000", "0.5", "123456789012", "null", "null" };
    for (size_t i = 0; i < sizeof vals / sizeof *vals; i++) {
        sb_init(&b); json_put_number(&b, vals[i]);
        CHECK_STR(b.s, want[i]);
        sb_free(&b);
    }
}

static void t_json_getters(void) {
    jv *v = json_parse("{\"s\":\"str\",\"n\":7,\"t\":true,\"f\":false,\"z\":null}", 45);
    CHECK(v != NULL);
    size_t l;
    CHECK_STR(json_gets(v, "s", &l), "str"); CHECK_INT(l, 3);
    CHECK(json_gets(v, "n", NULL) == NULL && json_gets(v, "missing", NULL) == NULL);
    CHECK_INT(json_getnum(v, "n", -1), 7); CHECK_INT(json_getnum(v, "s", -1), -1);
    CHECK_INT(json_getbool(v, "t", 0), 1); CHECK_INT(json_getbool(v, "f", 1), 0); CHECK_INT(json_getbool(v, "n", 9), 9);
    CHECK(json_get(json_get(v, "s"), "x") == NULL);                 /* not an object */
    json_free(v);
    CHECK(json_get(NULL, "x") == NULL);
}

static void t_file_reading(void) {
    char *p = tmp_write("rf.txt", "hello\nworld\0z", 13);
    size_t n; char *d = read_file(p, &n);
    CHECK(d != NULL && n == 13 && memcmp(d, "hello\nworld\0z", 13) == 0);
    free(d);
    CHECK(read_file("/nonexistent/definitely/not/here", &n) == NULL);
}

/* ---- edit.c ---- */
static void t_edit_offset(void) {
    const char *t = "h\xC3\xA9llo\nw\xC3\xB6rld\n";                  /* "héllo\nwörld\n": é and ö are 2 bytes each */
    size_t n = strlen(t);
    CHECK_INT(edit_offset(t, n, 1, 1), 0);
    CHECK_INT(edit_offset(t, n, 1, 3), 3);                         /* after h and é */
    CHECK_INT(edit_offset(t, n, 1, 6), 6);                         /* column length+1 is the end of the row */
    CHECK_INT(edit_offset(t, n, 1, 7), -1);                        /* but no further */
    CHECK_INT(edit_offset(t, n, 2, 1), 7);
    CHECK_INT(edit_offset(t, n, 2, 4), 11);
    CHECK_INT(edit_offset(t, n, 3, 1), n);                         /* the empty row after the final newline */
    CHECK_INT(edit_offset(t, n, 3, 2), -1);
    CHECK_INT(edit_offset(t, n, 4, 1), -1);
    CHECK_INT(edit_offset(t, n, 0, 1), -1); CHECK_INT(edit_offset(t, n, 1, 0), -1);
    CHECK_INT(edit_offset("ab", 2, 2, 1), 2);                      /* just past an unterminated last row */
    CHECK_INT(edit_offset("ab", 2, 1, 3), 2);
    CHECK_INT(edit_offset("", 0, 1, 1), 0);
}

static void t_edit_replace_and_endpos(void) {
    const char *t = "alpha\nbeta\ngamma\n";
    size_t n = strlen(t), outn, rn; char *removed;
    char *o = edit_replace(t, n, 2, 1, 2, 5, "BETA", 4, &outn, &removed, &rn);       /* within a row */
    CHECK_STR(o, "alpha\nBETA\ngamma\n"); CHECK_STR(removed, "beta"); CHECK_INT(rn, 4); free(o); free(removed);
    o = edit_replace(t, n, 1, 4, 3, 3, "-", 1, &outn, &removed, &rn);                /* across rows */
    CHECK_STR(o, "alp-mma\n"); CHECK_STR(removed, "ha\nbeta\nga"); free(o); free(removed);
    o = edit_replace(t, n, 2, 1, 3, 1, "", 0, &outn, &removed, &rn);                 /* a whole line, newline included */
    CHECK_STR(o, "alpha\ngamma\n"); CHECK_STR(removed, "beta\n"); free(o); free(removed);
    o = edit_replace(t, n, 5, 1, 5, 1, "x", 1, &outn, NULL, NULL); CHECK(o == NULL);  /* row 5 does not exist */
    o = edit_replace(t, n, 3, 1, 4, 1, "", 0, &outn, NULL, NULL);                    /* row 4 is the empty row after the final newline */
    CHECK_STR(o, "alpha\nbeta\n"); free(o);
    o = edit_replace(t, n, 3, 1, 2, 1, "x", 1, &outn, NULL, NULL); CHECK(o == NULL);  /* end before begin */
    o = edit_replace(t, n, 3, 6, 3, 6, "!", 1, &outn, NULL, NULL);                   /* insert at the end of a row */
    CHECK_STR(o, "alpha\nbeta\ngamma!\n"); free(o);
    long r2, c2;
    edit_endpos(2, 3, "xy", 2, &r2, &c2); CHECK_INT(r2, 2); CHECK_INT(c2, 5);
    edit_endpos(2, 3, "x\ny\xC3\xA9", 5, &r2, &c2); CHECK_INT(r2, 3); CHECK_INT(c2, 3);   /* two code points after the newline */
    edit_endpos(2, 3, "", 0, &r2, &c2); CHECK_INT(r2, 2); CHECK_INT(c2, 3);
    edit_endpos(2, 3, "a\n", 2, &r2, &c2); CHECK_INT(r2, 3); CHECK_INT(c2, 1);
}

/* ---- edits.c: the model's proposed edits ---- */
static edit_item *parse_ok(const char *json, size_t *n) {
    edit_item *e; const char *err = edits_parse(json, strlen(json), &e, n);
    if (err) FAIL("parse failed: %s", err);
    return e;
}

static void t_edits_parse(void) {
    size_t n; edit_item *e;
    e = parse_ok("{\"edits\":[{\"annotation\":7,\"find\":\"very good\",\"replace\":\"good\",\"why\":\"filler\"},"
                 "{\"find\":\"a\",\"replace\":\"\",\"also\":true}]}", &n);
    CHECK_INT(n, 2);
    CHECK_INT(e[0].annotation, 7); CHECK_STR(e[0].find, "very good"); CHECK_STR(e[0].replace, "good"); CHECK_STR(e[0].why, "filler"); CHECK_INT(e[0].also, 0);
    CHECK_INT(e[1].annotation, 0); CHECK_STR(e[1].replace, ""); CHECK_INT(e[1].also, 1);
    edits_free(e, n);
    e = parse_ok("Here are the edits:\n```json\n{\"edits\": [{\"annotation\": 1, \"find\": \"x\", \"replace\": \"y\"}]}\n```\nDone.", &n);   /* prose and a fence around it */
    CHECK_INT(n, 1); CHECK_STR(e[0].find, "x"); edits_free(e, n);
    e = parse_ok("[{\"find\":\"x\",\"replace\":\"y\"}]", &n); CHECK_INT(n, 1); edits_free(e, n);                              /* a bare list is fine too */
    e = parse_ok("{\"edits\":[{\"annotations\":[4,7,9],\"find\":\"x\",\"replace\":\"y\"},{\"annotations\":[5],\"find\":\"a\",\"replace\":\"b\"}]}", &n);
    CHECK_INT(n, 2); CHECK_INT(e[0].annotation, 4); CHECK_INT(e[0].nextra, 2); CHECK_INT(e[0].extra[0], 7); CHECK_INT(e[0].extra[1], 9);   /* one edit, several notes */
    CHECK_INT(e[1].annotation, 5); CHECK_INT(e[1].nextra, 0); edits_free(e, n);
    e = parse_ok("{\"edits\":[]}", &n); CHECK_INT(n, 0); edits_free(e, n);                                                  /* nothing to change is an answer */
    e = parse_ok("{\"edits\":[{\"find\":\"caf\\u00e9\",\"replace\":\"cafe\\n\"}]}", &n);
    CHECK_STR(e[0].find, "caf\xC3\xA9"); CHECK_STR(e[0].replace, "cafe\n"); edits_free(e, n);

    edit_item *bad; size_t bn;
    CHECK(edits_parse("no json here", 12, &bad, &bn) != NULL);
    CHECK(edits_parse("{\"edits\": [oops", 15, &bad, &bn) != NULL);
    CHECK(edits_parse("{\"other\": 1}", 12, &bad, &bn) != NULL);                                                            /* no edits list */
    CHECK(edits_parse("{\"edits\":[{\"find\":\"x\"}]}", 24, &bad, &bn) != NULL);                                           /* an edit with no replace */
}

static void t_edits_verify(void) {
    const char *text = "The cat sat. The dog sat. A very good day, a very good night.\nSecond line: keep me.\n";
    size_t tl = strlen(text), n; edit_item *e;
    e = parse_ok("{\"edits\":["
        "{\"annotation\":1,\"find\":\"The cat\",\"replace\":\"A cat\"},"            /* ok */
        "{\"annotation\":2,\"find\":\"nowhere\",\"replace\":\"x\"},"                 /* not found */
        "{\"annotation\":3,\"find\":\" sat.\",\"replace\":\" ran.\"},"                /* twice */
        "{\"annotation\":4,\"find\":\"\",\"replace\":\"x\"},"                         /* names nothing */
        "{\"annotation\":5,\"find\":\"dog\",\"replace\":\"dog\"},"                    /* identical */
        "{\"annotation\":6,\"find\":\"keep me\",\"replace\":\"drop me\"},"            /* touches a keep span */
        "{\"annotation\":7,\"find\":\"The cat sat\",\"replace\":\"It sat\"}"          /* overlaps the first */
        "]}", &n);
    const char *kp = strstr(text, "keep me");
    keep_span keep[1] = { { 99, (size_t)(kp - text), 7 } };
    size_t ok = edits_verify(text, tl, e, n, keep, 1);
    CHECK_INT(ok, 1);
    CHECK_INT(e[0].ok, 1); CHECK_INT(e[0].off, 0); CHECK_INT(e[0].len, 7);
    CHECK_CONTAINS(e[1].problem, "not found"); CHECK_CONTAINS(e[2].problem, "2 times"); CHECK_CONTAINS(e[3].problem, "no text");
    CHECK_CONTAINS(e[4].problem, "identical"); CHECK_CONTAINS(e[5].problem, "#99"); CHECK_CONTAINS(e[6].problem, "overlaps another");
    CHECK_INT(e[1].ok + e[2].ok + e[3].ok + e[4].ok + e[5].ok + e[6].ok, 0);
    edits_free(e, n);
    e = parse_ok("{\"edits\":[{\"find\":\"very good day\",\"replace\":\"fine day\"}]}", &n);       /* found once: the other 'very good' is followed by 'night' */
    CHECK_INT(edits_verify(text, tl, e, n, NULL, 0), 1); edits_free(e, n);
    e = parse_ok("{\"edits\":[{\"find\":\"very good\",\"replace\":\"fine\"}]}", &n);               /* twice: ambiguous, never guessed at */
    CHECK_INT(edits_verify(text, tl, e, n, NULL, 0), 0); CHECK_CONTAINS(e[0].problem, "ambiguous"); edits_free(e, n);
}

static void t_edits_islands_and_apply(void) {
    const char *text = "First line is plain.\nSecond has very very bad and also quite poor words.\nThird line stays.\nFourth: caf\xC3\xA9 au lait.\n";
    size_t tl = strlen(text), n; edit_item *e;
    e = parse_ok("{\"edits\":["
        "{\"annotation\":1,\"find\":\"very very bad\",\"replace\":\"bad\"},"
        "{\"annotation\":2,\"find\":\"quite poor\",\"replace\":\"poor\"},"                    /* same line as the first: one island */
        "{\"annotation\":3,\"find\":\"caf\xC3\xA9 au lait\",\"replace\":\"tea\"}"            /* a multibyte line on its own */
        "]}", &n);
    CHECK_INT(edits_verify(text, tl, e, n, NULL, 0), 3);
    island *is; size_t ni = edits_islands(text, tl, e, n, &is);
    CHECK_INT(ni, 2);
    CHECK_INT(is[0].r1, 2); CHECK_INT(is[0].r2, 2); CHECK_INT(is[0].nidx, 2);
    CHECK_STR(is[0].old_text, "Second has very very bad and also quite poor words.");
    CHECK_STR(is[0].new_text, "Second has bad and also poor words.");
    CHECK_INT(is[1].r1, 4); CHECK_INT(is[1].nidx, 1);
    CHECK_STR(is[1].old_text, "Fourth: caf\xC3\xA9 au lait."); CHECK_STR(is[1].new_text, "Fourth: tea.");

    size_t out; int accept[2];
    accept[0] = 1; accept[1] = 1; char *all = edits_apply(text, tl, e, is, ni, accept, &out);
    CHECK_STR(all, "First line is plain.\nSecond has bad and also poor words.\nThird line stays.\nFourth: tea.\n"); free(all);
    accept[0] = 0; accept[1] = 1; char *only2 = edits_apply(text, tl, e, is, ni, accept, &out);                 /* reject the first island */
    CHECK_STR(only2, "First line is plain.\nSecond has very very bad and also quite poor words.\nThird line stays.\nFourth: tea.\n"); free(only2);
    accept[0] = 1; accept[1] = 0; char *only1 = edits_apply(text, tl, e, is, ni, accept, &out);
    CHECK_STR(only1, "First line is plain.\nSecond has bad and also poor words.\nThird line stays.\nFourth: caf\xC3\xA9 au lait.\n"); free(only1);
    accept[0] = 0; accept[1] = 0; char *none = edits_apply(text, tl, e, is, ni, accept, &out);                  /* nothing accepted: the text is untouched */
    CHECK_STR(none, text); CHECK_INT(out, tl); free(none);
    islands_free(is, ni); edits_free(e, n);

    /* a find that spans a line break covers both lines; the result is still correct */
    e = parse_ok("{\"edits\":[{\"find\":\"plain.\\nSecond\",\"replace\":\"plain. Second\"}]}", &n);
    CHECK_INT(edits_verify(text, tl, e, n, NULL, 0), 1);
    ni = edits_islands(text, tl, e, n, &is); CHECK_INT(ni, 1); CHECK_INT(is[0].r1, 1); CHECK_INT(is[0].r2, 2);
    CHECK_STR(is[0].new_text, "First line is plain. Second has very very bad and also quite poor words.");
    accept[0] = 1; char *joined = edits_apply(text, tl, e, is, ni, accept, &out);
    CHECK_CONTAINS(joined, "plain. Second has"); free(joined);
    islands_free(is, ni); edits_free(e, n);

    /* an edit that deletes (empty replacement) and one at the very start and very end of the text */
    e = parse_ok("{\"edits\":[{\"find\":\"First\",\"replace\":\"1st\"},{\"find\":\"au lait.\",\"replace\":\"\"}]}", &n);
    CHECK_INT(edits_verify(text, tl, e, n, NULL, 0), 2);
    ni = edits_islands(text, tl, e, n, &is); CHECK_INT(ni, 2);
    accept[0] = 1; accept[1] = 1; char *ends = edits_apply(text, tl, e, is, ni, accept, &out);
    CHECK_STR(ends, "1st line is plain.\nSecond has very very bad and also quite poor words.\nThird line stays.\nFourth: caf\xC3\xA9 \n"); free(ends);
    islands_free(is, ni); edits_free(e, n);
}

/* ---- llm.c ---- */
static void t_llm_prompt_and_command(void) {
    const char *sys = llm_system_prompt();
    CHECK_CONTAINS(sys, "slop"); CHECK_CONTAINS(sys, "keep"); CHECK_CONTAINS(sys, "note");                       /* the three kinds are explained */
    CHECK_CONTAINS(sys, "exactly once"); CHECK_CONTAINS(sys, "{\"edits\":[]}"); CHECK_CONTAINS(sys, "Do not add facts");
    sb_t u; sb_init(&u);
    llm_user_message(&u, "Line one.\nLine two.", 19, "[{\"id\":1,\"kind\":\"slop\"}]");
    CHECK_CONTAINS(u.s, "<document>\nLine one.\nLine two.\n</document>");                                         /* a final newline is added if missing */
    CHECK_CONTAINS(u.s, "<annotations>\n[{\"id\":1,\"kind\":\"slop\"}]\n</annotations>");
    sb_t rq; sb_init(&rq); llm_request_json(&rq, "SYS \"q\"", u.s, u.len, "some-model");
    jv *j = json_parse(rq.s, rq.len); CHECK(j != NULL);
    CHECK_STR(json_gets(j, "system", NULL), "SYS \"q\""); CHECK_STR(json_gets(j, "user", NULL), u.s); CHECK_STR(json_gets(j, "model", NULL), "some-model");
    json_free(j); sb_free(&rq); sb_free(&u);

    unsetenv("WINLESS_CLAUDE");
    char **a = llm_claude_argv("THE PROMPT", NULL, NULL);
    CHECK_STR(a[0], "claude"); CHECK_STR(a[1], "-p");
    CHECK_STR(a[2], "--tools"); CHECK_STR(a[3], "");                                                              /* no tools: it can only answer */
    CHECK_STR(a[4], "--no-session-persistence");                                                                  /* nothing kept afterwards */
    CHECK_STR(a[5], "--system-prompt"); CHECK_STR(a[6], "THE PROMPT"); CHECK(a[7] == NULL);
    llm_free_argv(a);
    setenv("WINLESS_CLAUDE", "/opt/fake-claude", 1);
    a = llm_claude_argv("P", "claude-opus-5-5", "high");
    CHECK_STR(a[0], "/opt/fake-claude"); CHECK_STR(a[7], "--model"); CHECK_STR(a[8], "claude-opus-5-5"); CHECK_STR(a[9], "--effort"); CHECK_STR(a[10], "high"); CHECK(a[11] == NULL);
    llm_free_argv(a); unsetenv("WINLESS_CLAUDE");
}

int main(void) {
    testlib_init_noserver();
    static const testcase tests[] = {
        { "sb_basics", t_sb_basics, 0 },
        { "utf8_valid_is_kept", t_utf8_valid_is_kept, 0 },
        { "utf8_invalid_becomes_fffd", t_utf8_invalid_becomes_fffd, 0 },
        { "utf8_nul_becomes_fffd", t_utf8_nul_becomes_fffd, 0 },
        { "json_parses_structures", t_json_parses_structures, 0 },
        { "json_string_escapes", t_json_string_escapes, 0 },
        { "json_numbers", t_json_numbers, 0 },
        { "json_rejects_bad_input", t_json_rejects_bad_input, 0 },
        { "json_depth_limit", t_json_depth_limit, 0 },
        { "json_write_is_compact_and_roundtrips", t_json_write_is_compact_and_roundtrips, 0 },
        { "json_put_string_and_number", t_json_put_string_and_number, 0 },
        { "json_getters", t_json_getters, 0 },
        { "file_reading", t_file_reading, 0 },
        { "edit_offset", t_edit_offset, 0 },
        { "edit_replace_and_endpos", t_edit_replace_and_endpos, 0 },
        { "edits_parse", t_edits_parse, 0 },
        { "edits_verify", t_edits_verify, 0 },
        { "edits_islands_and_apply", t_edits_islands_and_apply, 0 },
        { "llm_prompt_and_command", t_llm_prompt_and_command, 0 },
    };
    return run_tests(tests, sizeof tests / sizeof *tests) ? 1 : 0;
}
