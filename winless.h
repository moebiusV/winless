/* winless.h -- shared declarations.  Copyright (C) 2026 David Walther.  GPL-3.0-or-later. */
#ifndef WINLESS_H
#define WINLESS_H

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
void winless_data_dir(char *buf, size_t n);                /* ~/.local/share/winless (or $XDG_DATA_HOME/winless), created */
void winless_cache_dir(char *buf, size_t n);               /* ~/.cache/winless (or $XDG_CACHE_HOME/winless), created */
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

/* ---- edit.c ---- (rows from 1; columns in code points from 1; a range excludes its end) */
size_t edit_offset(const char *text, size_t len, long row, long col);      /* byte offset, or (size_t)-1 if out of range */
char *edit_replace(const char *text, size_t len, long r1, long c1, long r2, long c2, const char *repl, size_t rlen,
                   size_t *outlen, char **removed, size_t *removedlen);    /* new text (caller frees), NULL if the range is invalid */
void edit_endpos(long r1, long c1, const char *repl, size_t rlen, long *r2, long *c2);   /* where text inserted at (r1,c1) ends */

/* ---- store.c ---- (the annotation database; each returns NULL on success, else an error message) */
const char *store_add(const char *file, long r1, long c1, long r2, long c2, const char *scope, const char *selected,
                      const char *paragraphs, const char *note, long *id);      /* r1 == 0: no position (scope "file") */
const char *store_list_json(sb_t *out, const char *file, int include_deleted);  /* a JSON array; file == NULL: every file */
const char *store_set_deleted(long id, int deleted);                            /* hide a note, or bring it back */

/* ---- server.c ---- */
int server_run(int port, double idle_secs);              /* serve until stopped (or, if idle_secs > 0, until no window has been connected and no edits are unsaved for that long) */

/* ---- client.c ---- */
int client_main(int port, int explicit_port, int argc, char **argv);   /* the CLI commands; explicit_port: --port or $WINLESS_PORT was given */
int server_alive(int port);
const char *viewer_html_path(void);

#endif
