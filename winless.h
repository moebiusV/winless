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
void winless_port_file(int port, char *buf, size_t n, int create);   /* ~/.cache/winless/ports/PORT (line 1: pid, line 2: the server's secret); create: make the 0700 directory */
int winless_port_token(int port, char *buf, size_t n);     /* the secret of the server on PORT (read from its registry file); 0 if there is none */
int token_make(char *buf, size_t n);                       /* a fresh random secret as 64 hex digits (n >= 65); 0 on failure */

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
                      const char *paragraphs, const char *note, const char *kind, long *id);   /* r1 == 0: no position (scope "file"); kind: note | slop | keep */
const char *store_set_status(long id, const char *status);
const char *store_add_review(const char *file, const char *backend, const char *model, const char *scope, const char *annotations,
                             long accepted, long rejected, long pending, int written, const char *commit, const char *note);                      /* open | done */
const char *store_list_json(sb_t *out, const char *file, int include_deleted);  /* a JSON array; file == NULL: every file */
const char *store_set_deleted(long id, int deleted);                            /* hide a note, or bring it back */

/* ---- edits.c ---- (the model's proposed edits: parse, verify, group into islands, apply) */
typedef struct {
    long annotation; int also;              /* the note this answers (0: none); also: an extra spot found for a slop mark */
    long extra[8]; int nextra;              /* more notes the same edit answers (when two notes touch the same or adjacent words) */
    char *find, *replace, *why;             /* replace the exact text `find` with `replace`, because `why` */
    int ok; size_t off, len; char *problem; /* after edits_verify: ok and the byte range found, or why it could not be applied */
} edit_item;
typedef struct { long id; size_t off, len; } keep_span;      /* text the author marked keep: no edit may touch it */
typedef struct {
    long r1, r2;                            /* the original lines the island covers (1-based, inclusive) */
    size_t off, len;                        /* their bytes in the original (without the final newline) */
    size_t *idx; size_t nidx;               /* the edits in it */
    char *old_text, *new_text;              /* those lines before and after the edits */
} island;
const char *edits_parse(const char *reply, size_t len, edit_item **out, size_t *n);    /* NULL, or an error message */
void edits_free(edit_item *e, size_t n);
size_t edits_verify(const char *text, size_t tlen, edit_item *e, size_t n, const keep_span *keep, size_t nkeep);   /* returns how many are ok */
size_t edits_islands(const char *text, size_t tlen, const edit_item *e, size_t n, island **out);
void islands_free(island *is, size_t n);
char *edits_apply(const char *text, size_t tlen, const edit_item *e, const island *is, size_t nis, const int *accept, size_t *outlen);

/* ---- llm.c ---- (what the model is told, and the command line of the claude backend; nothing here runs a process) */
const char *llm_system_prompt(void);
void llm_user_message(sb_t *out, const char *doc, size_t doclen, const char *annotations_json);
void llm_request_json(sb_t *out, const char *system, const char *user, size_t userlen, const char *model);
char **llm_claude_argv(const char *system, const char *model, const char *effort);
void llm_free_argv(char **a);

/* ---- server.c ---- */
int server_run(int port, double idle_secs);              /* serve until stopped (or, if idle_secs > 0, until no window has been connected and no edits are unsaved for that long) */

/* ---- client.c ---- */
int client_main(int port, int explicit_port, int argc, char **argv);   /* the CLI commands; explicit_port: --port or $WINLESS_PORT was given */
int server_alive(int port);
const char *viewer_html_path(void);
int open_url_window(const char *url, int windowed);                    /* a chromeless browser window on URL */
int free_local_port(void);                                             /* a currently unused loopback port, or 0 */

/* ---- remote.c ---- (winless over ssh: the desktop side runs `winless ssh`, which keeps a relay; the remote side rings its bell) */
int remote_ssh_main(int argc, char **argv);            /* winless ssh [ssh-options] HOST [command] */
int remote_relay_main(int argc, char **argv);          /* winless relay DIR CTL DEST BELL   (started by `winless ssh`, not by hand) */
int bell_available(void);                              /* true inside `winless ssh`: $WINLESS_BELL names a socket that answers */
int bell_call(const char *line, size_t n, char **reply);   /* send one request line to the relay; 0 on success, *reply is its answer */

#endif
