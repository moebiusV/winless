/* edits.c -- the model's proposed edits: parse, verify against the real text, group into islands, apply.
 * Copyright (C) 2026 David Walther.  GPL-3.0-or-later.
 *
 * The model does not rewrite a file.  It answers with a list of edits, each "find this exact text, replace it with that, because ...".
 * Nothing is trusted: an edit is shown for review only if its `find` text occurs exactly once in the file and does not touch a span the
 * author marked "keep" or another edit.  Anything else is reported as "could not apply", with the reason, and is never guessed at.
 * Edits that share a line are grouped into one island (the unit the author accepts or rejects).
 */
#define _POSIX_C_SOURCE 200809L
#include "winless.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_perr[200];

void edits_free(edit_item *e, size_t n) {
    for (size_t i = 0; i < n; i++) { free(e[i].find); free(e[i].replace); free(e[i].why); free(e[i].problem); }
    free(e);
}

/* The reply may have prose or a code fence around the JSON: take from the first '{' (or '[') to the matching last '}' (or ']'). */
const char *edits_parse(const char *reply, size_t len, edit_item **out, size_t *n) {
    *out = NULL; *n = 0;
    size_t a = 0, b = len;
    while (a < len && reply[a] != '{' && reply[a] != '[') a++;
    while (b > a && reply[b - 1] != '}' && reply[b - 1] != ']') b--;
    if (a >= b) return "the reply contains no JSON";
    jv *root = json_parse(reply + a, b - a);
    if (!root) return "the reply is not valid JSON";
    const jv *list = root->t == J_ARR ? root : json_get(root, "edits");
    if (!list || list->t != J_ARR) { json_free(root); return "the reply has no \"edits\" list"; }
    edit_item *e = xmalloc((list->n ? list->n : 1) * sizeof *e);
    size_t k = 0;
    for (size_t i = 0; i < list->n; i++) {
        const jv *it = list->items[i];
        if (!it || it->t != J_OBJ) continue;
        size_t fl = 0, rl = 0;
        const char *find = json_gets(it, "find", &fl), *rep = json_gets(it, "replace", &rl), *why = json_gets(it, "why", NULL);
        if (!find || !rep) { snprintf(g_perr, sizeof g_perr, "edit %zu lacks find or replace", i + 1); edits_free(e, k); json_free(root); return g_perr; }
        memset(&e[k], 0, sizeof e[k]);
        e[k].annotation = (long)json_getnum(it, "annotation", 0);
        const jv *ids = json_get(it, "annotations");                    /* an edit may answer several notes: the first is the main one */
        if (ids && ids->t == J_ARR)
            for (size_t q = 0; q < ids->n; q++) {
                if (ids->items[q]->t != J_NUM) continue;
                long id = (long)ids->items[q]->num;
                if (!e[k].annotation) e[k].annotation = id;
                else if (id != e[k].annotation && e[k].nextra < 8) e[k].extra[e[k].nextra++] = id;
            }
        e[k].also = json_getbool(it, "also", 0);
        e[k].find = xstrndup(find, fl); e[k].replace = xstrndup(rep, rl); e[k].why = xstrdup(why ? why : "");
        k++;
    }
    json_free(root);
    *out = e; *n = k;
    return NULL;
}

static size_t count_occurrences(const char *text, size_t tlen, const char *find, size_t fl, size_t *first) {
    size_t n = 0;
    if (!fl || fl > tlen) return 0;
    for (size_t i = 0; i + fl <= tlen; i++) {
        if (text[i] == find[0] && memcmp(text + i, find, fl) == 0) { if (n == 0) *first = i; n++; }
    }
    return n;
}
static void set_problem(edit_item *e, const char *msg) { free(e->problem); e->problem = xstrdup(msg); e->ok = 0; }

size_t edits_verify(const char *text, size_t tlen, edit_item *e, size_t n, const keep_span *keep, size_t nkeep) {
    for (size_t i = 0; i < n; i++) {
        size_t fl = strlen(e[i].find), first = 0; e[i].ok = 0; e[i].off = e[i].len = 0;
        free(e[i].problem); e[i].problem = NULL;
        if (!fl) { set_problem(&e[i], "it names no text to change"); continue; }
        size_t occ = count_occurrences(text, tlen, e[i].find, fl, &first);
        if (occ == 0) { set_problem(&e[i], "the text to change was not found exactly as written"); continue; }
        if (occ > 1) { char m[96]; snprintf(m, sizeof m, "the text to change appears %zu times, so it is ambiguous", occ); set_problem(&e[i], m); continue; }
        if (strcmp(e[i].find, e[i].replace) == 0) { set_problem(&e[i], "the replacement is identical to the text"); continue; }
        int blocked = 0;
        for (size_t k = 0; k < nkeep && !blocked; k++)
            if (first < keep[k].off + keep[k].len && keep[k].off < first + fl) {
                char m[96]; snprintf(m, sizeof m, "it overlaps text you marked keep (#%ld)", keep[k].id); set_problem(&e[i], m); blocked = 1;
            }
        if (blocked) continue;
        e[i].ok = 1; e[i].off = first; e[i].len = fl;
    }
    /* two edits must not touch the same text: the earlier position (then the earlier in the list) wins */
    for (size_t i = 0; i < n; i++) {
        if (!e[i].ok) continue;
        for (size_t j = 0; j < n; j++) {
            if (j == i || !e[j].ok) continue;
            int i_first = e[i].off < e[j].off || (e[i].off == e[j].off && i < j);
            if (!i_first) continue;
            if (e[j].off < e[i].off + e[i].len) { set_problem(&e[j], "it overlaps another proposed change"); }
        }
    }
    size_t ok = 0; for (size_t i = 0; i < n; i++) ok += (size_t)e[i].ok;
    return ok;
}

static const edit_item *g_sort_edits;
static int cmp_by_off(const void *a, const void *b) {            /* by position in the text, then by order in the list */
    size_t ia = *(const size_t *)a, ib = *(const size_t *)b;
    const edit_item *x = &g_sort_edits[ia], *y = &g_sort_edits[ib];
    if (x->off != y->off) return x->off < y->off ? -1 : 1;
    return ia < ib ? -1 : ia > ib ? 1 : 0;
}

static long row_of(const char *text, size_t off) { long r = 1; for (size_t i = 0; i < off; i++) if (text[i] == '\n') r++; return r; }
static size_t line_start(const char *text, size_t off) { while (off > 0 && text[off - 1] != '\n') off--; return off; }
static size_t line_end(const char *text, size_t tlen, size_t off) { while (off < tlen && text[off] != '\n') off++; return off; }

/* Group the verified edits into islands: edits that share a line are one island.  Each island knows the whole lines it covers, before
 * and after its edits are applied, so the author sees a paragraph change as a paragraph, not as a fragment. */
size_t edits_islands(const char *text, size_t tlen, const edit_item *e, size_t n, island **out) {
    size_t *idx = xmalloc((n ? n : 1) * sizeof *idx), m = 0;
    for (size_t i = 0; i < n; i++) if (e[i].ok) idx[m++] = i;
    g_sort_edits = e;
    qsort(idx, m, sizeof *idx, cmp_by_off);
    island *is = xmalloc((m ? m : 1) * sizeof *is); size_t ni = 0;
    for (size_t k = 0; k < m; k++) {
        const edit_item *ed = &e[idx[k]];
        size_t ls = line_start(text, ed->off), le = line_end(text, tlen, ed->off + (ed->len ? ed->len - 1 : 0));
        if (ni && ls <= is[ni - 1].off + is[ni - 1].len) {                       /* shares a line with the previous island */
            island *p = &is[ni - 1];
            p->idx = xrealloc(p->idx, (p->nidx + 1) * sizeof *p->idx); p->idx[p->nidx++] = idx[k];
            if (le > p->off + p->len) p->len = le - p->off;
            continue;
        }
        island *q = &is[ni++]; memset(q, 0, sizeof *q);
        q->idx = xmalloc(sizeof *q->idx); q->idx[0] = idx[k]; q->nidx = 1; q->off = ls; q->len = le - ls;
    }
    for (size_t g = 0; g < ni; g++) {
        island *p = &is[g];
        p->old_text = xstrndup(text + p->off, p->len);
        sb_t nt; sb_init(&nt); size_t pos = p->off;
        for (size_t k = 0; k < p->nidx; k++) {
            const edit_item *ed = &e[p->idx[k]];
            sb_put(&nt, text + pos, ed->off - pos); sb_puts(&nt, ed->replace); pos = ed->off + ed->len;
        }
        sb_put(&nt, text + pos, p->off + p->len - pos);
        p->new_text = sb_detach(&nt, NULL);
        p->r1 = row_of(text, p->off);
        p->r2 = row_of(text, p->len ? p->off + p->len - 1 : p->off);
    }
    free(idx);
    *out = is;
    return ni;
}

void islands_free(island *is, size_t n) {
    for (size_t i = 0; i < n; i++) { free(is[i].idx); free(is[i].old_text); free(is[i].new_text); }
    free(is);
}

/* The text with the edits of every accepted island applied (accept[g] != 0 for island g). */
char *edits_apply(const char *text, size_t tlen, const edit_item *e, const island *is, size_t nis, const int *accept, size_t *outlen) {
    size_t n = 0;
    for (size_t g = 0; g < nis; g++) n += is[g].nidx;
    size_t *idx = xmalloc((n ? n : 1) * sizeof *idx), m = 0;
    for (size_t g = 0; g < nis; g++) if (accept[g]) for (size_t k = 0; k < is[g].nidx; k++) idx[m++] = is[g].idx[k];
    g_sort_edits = e;
    qsort(idx, m, sizeof *idx, cmp_by_off);
    sb_t out; sb_init(&out); size_t pos = 0;
    for (size_t k = 0; k < m; k++) {
        const edit_item *ed = &e[idx[k]];
        sb_put(&out, text + pos, ed->off - pos); sb_puts(&out, ed->replace); pos = ed->off + ed->len;
    }
    sb_put(&out, text + pos, tlen - pos);
    free(idx);
    return sb_detach(&out, outlen);
}
