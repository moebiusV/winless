/* winview.h -- shared declarations.  Copyright (C) 2026 David Walther.  GPL-3.0-or-later. */
#ifndef WINVIEW_H
#define WINVIEW_H

#include <stddef.h>

#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "0.0"
#endif
#define DEFAULT_PORT 8777

/* ---- util.c ---- */
typedef struct { char *s; size_t len, cap; } sb_t;       /* growable byte buffer, always NUL-terminated */
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
void sb_init(sb_t *b);
void sb_free(sb_t *b);
void sb_put(sb_t *b, const char *s, size_t n);
void sb_puts(sb_t *b, const char *s);
void sb_putc(sb_t *b, char c);
void sb_printf(sb_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
char *sb_detach(sb_t *b, size_t *len);
void sb_put_utf8_clean(sb_t *b, const char *s, size_t n);  /* copy, turning invalid UTF-8 into U+FFFD */
double now_monotonic(void);
char *read_file(const char *path, size_t *len);            /* NULL on error */

/* ---- json.c ---- */
typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype;
typedef struct jv {
    jtype t;
    double num;
    int b;
    char *str; size_t slen;                 /* J_STR (NUL-terminated, may contain NULs: use slen) */
    struct jv **items; char **keys; size_t n;   /* J_ARR / J_OBJ (keys only for J_OBJ) */
} jv;
jv *json_parse(const char *s, size_t len);               /* NULL on a syntax error */
void json_free(jv *v);
const jv *json_get(const jv *obj, const char *key);      /* NULL if absent or obj is not an object */
const char *json_gets(const jv *obj, const char *key, size_t *len);   /* string value or NULL */
double json_getnum(const jv *obj, const char *key, double dflt);
int json_getbool(const jv *obj, const char *key, int dflt);
void json_put_string(sb_t *b, const char *s, size_t n);  /* append a quoted, escaped JSON string */
void json_put_number(sb_t *b, double d);
void json_write(sb_t *b, const jv *v);

/* ---- server.c ---- */
int server_run(int port);                                /* serve until stopped */

/* ---- client.c ---- */
int client_main(int port, int argc, char **argv);        /* the CLI commands */
int server_alive(int port);
const char *viewer_html_path(void);

#endif
