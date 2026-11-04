/* testlib.h -- a tiny test harness plus helpers to run the winless binary as a black box.
 * Copyright (C) 2026 David Walther.  GPL-3.0-or-later. */
#ifndef TESTLIB_H
#define TESTLIB_H

#include "winless.h"
#include <stddef.h>
#include <sys/types.h>

/* ---- harness ---- */
typedef struct { const char *name; void (*fn)(void); int needs_server; } testcase;
int run_tests(const testcase *tests, size_t n);                 /* returns the number of failures */
void test_fail(const char *file, int line, const char *fmt, ...) __attribute__((noreturn, format(printf, 3, 4)));

#define FAIL(...) test_fail(__FILE__, __LINE__, __VA_ARGS__)
#define CHECK(c) do { if (!(c)) FAIL("check failed: %s", #c); } while (0)
#define CHECK_INT(a, b) do { long long _a = (long long)(a), _b = (long long)(b); if (_a != _b) FAIL("%s == %s: got %lld, wanted %lld", #a, #b, _a, _b); } while (0)
#define CHECK_STR(a, b) do { const char *_a = (a), *_b = (b); if (!_a || !_b || strcmp(_a, _b)) FAIL("%s == %s:\n  got    [%.300s]\n  wanted [%.300s]", #a, #b, _a ? _a : "(null)", _b ? _b : "(null)"); } while (0)
#define CHECK_CONTAINS(hay, needle) do { const char *_h = (hay), *_n = (needle); if (!_h || !strstr(_h, _n)) FAIL("expected to find [%s] in [%.300s]", _n, _h ? _h : "(null)"); } while (0)

/* ---- paths and environment ---- */
extern const char *g_bin;          /* the winless executable under test ($WINLESS_BIN) */
extern int g_port;                 /* the port of the current test's server */
void testlib_init(void);           /* sets HOME to a private directory, finds the binary under test */
void testlib_init_noserver(void);  /* the same without needing the binary (unit tests) */
const char *tmp_dir(void);
char *tmp_write(const char *name, const char *data, size_t len);     /* write a file in the temp dir; returns its path */
void msleep(int ms);
int free_port(void);

/* ---- the server under test ---- */
void server_start(void);           /* starts `winless serve` on a fresh port, waits until it answers */
void server_stop(void);            /* terminates it and fails if it printed sanitizer/UB reports */
int server_exited(double timeout); /* 1 if the server process has exited within `timeout` seconds */

/* ---- HTTP ---- */
typedef struct { int status; char *body; size_t len; } resp_t;
resp_t http(const char *method, const char *path, const char *body, size_t blen);
char *get_ok(const char *path);                               /* body of a 200 GET (malloc'ed) */
char *post_ok(const char *path, const char *body);            /* body of a 200 POST */
int post_status(const char *path, const char *body);
jv *get_json(const char *path);                               /* parsed 200 GET */
long clients(void);                                           /* connected pages, from /status */
void wait_clients(long n);

/* ---- Server-Sent Events client ---- */
typedef struct sse sse_t;
sse_t *sse_open(void);                                        /* connects to /events and waits for the stream to open */
size_t sse_wait(sse_t *s, size_t n, double timeout);          /* wait for at least n events; returns how many arrived */
const char *sse_event(sse_t *s, size_t i);                    /* the data: payload of event i, or NULL */
void sse_close(sse_t *s);

/* ---- raw sockets ---- */
int raw_connect(void);                                        /* a plain TCP connection to the server */
void raw_send(int fd, const char *data, size_t len);
char *raw_read(int fd, double timeout, size_t *len);          /* read until the server closes or the timeout passes */
void raw_close(int fd);

/* ---- the command line ---- */
typedef struct { int code; char *out; size_t outlen; } run_t;
/* run `winless --port PORT ARGS...` (argv after the program name, NULL-terminated); input is fed to stdin */
run_t cli_run(const char *input, size_t inlen, char *const args[]);
#define CLI(...) cli_run(NULL, 0, (char *const[]){ __VA_ARGS__, NULL })
run_t cli_auto(char *const args[]);                            /* no --port: the command finds or starts servers itself */
#define CLI_AUTO(...) cli_auto((char *const[]){ __VA_ARGS__, NULL })
#define CLI_IN(input, len, ...) cli_run((input), (len), (char *const[]){ __VA_ARGS__, NULL })
pid_t cli_spawn(char *const args[], int *out_fd);             /* start without waiting (stdout on *out_fd) */
run_t cli_finish(pid_t pid, int out_fd, double timeout);      /* collect a spawned command's output and exit code */

#endif
