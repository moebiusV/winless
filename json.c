/* json.c -- a small JSON parser and writer.  Copyright (C) 2026 David Walther.  GPL-3.0-or-later. */
#define _POSIX_C_SOURCE 200809L
#include "winview.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 64

typedef struct { const char *s; size_t n, i; int err; } cur_t;

static void skip_ws(cur_t *c) {
    while (c->i < c->n && (c->s[c->i] == ' ' || c->s[c->i] == '\t' || c->s[c->i] == '\n' || c->s[c->i] == '\r')) c->i++;
}

static jv *jnew(jtype t) { jv *v = xmalloc(sizeof *v); memset(v, 0, sizeof *v); v->t = t; return v; }

void json_free(jv *v) {
    if (!v) return;
    free(v->str);
    for (size_t i = 0; i < v->n; i++) {
        json_free(v->items[i]);
        if (v->keys) free(v->keys[i]);
    }
    free(v->items); free(v->keys); free(v);
}

static int hex4(cur_t *c, unsigned *out) {
    if (c->i + 4 > c->n) return 0;
    unsigned v = 0;
    for (int k = 0; k < 4; k++) {
        char ch = c->s[c->i + (size_t)k]; v <<= 4;
        if (ch >= '0' && ch <= '9') v |= (unsigned)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') v |= (unsigned)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') v |= (unsigned)(ch - 'A' + 10);
        else return 0;
    }
    c->i += 4; *out = v; return 1;
}

static void put_utf8(sb_t *b, unsigned cp) {
    char t[4];
    if (cp < 0x80) { t[0] = (char)cp; sb_put(b, t, 1); }
    else if (cp < 0x800) { t[0] = (char)(0xC0 | (cp >> 6)); t[1] = (char)(0x80 | (cp & 0x3F)); sb_put(b, t, 2); }
    else if (cp < 0x10000) { t[0] = (char)(0xE0 | (cp >> 12)); t[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); t[2] = (char)(0x80 | (cp & 0x3F)); sb_put(b, t, 3); }
    else { t[0] = (char)(0xF0 | (cp >> 18)); t[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); t[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); t[3] = (char)(0x80 | (cp & 0x3F)); sb_put(b, t, 4); }
}

/* Parses a string starting at the opening quote; the result is in *out (len in *olen). */
static int parse_string(cur_t *c, char **out, size_t *olen) {
    if (c->i >= c->n || c->s[c->i] != '"') return 0;
    c->i++;
    sb_t b; sb_init(&b);
    while (c->i < c->n) {
        unsigned char ch = (unsigned char)c->s[c->i++];
        if (ch == '"') { *out = sb_detach(&b, olen); return 1; }
        if (ch < 0x20) break;
        if (ch != '\\') { sb_putc(&b, (char)ch); continue; }
        if (c->i >= c->n) break;
        char e = c->s[c->i++];
        switch (e) {
        case '"': sb_putc(&b, '"'); break;   case '\\': sb_putc(&b, '\\'); break;  case '/': sb_putc(&b, '/'); break;
        case 'b': sb_putc(&b, '\b'); break;  case 'f': sb_putc(&b, '\f'); break;   case 'n': sb_putc(&b, '\n'); break;
        case 'r': sb_putc(&b, '\r'); break;  case 't': sb_putc(&b, '\t'); break;
        case 'u': {
            unsigned cp;
            if (!hex4(c, &cp)) goto bad;
            if (cp >= 0xD800 && cp <= 0xDBFF) {                    /* surrogate pair */
                unsigned lo;
                if (c->i + 2 <= c->n && c->s[c->i] == '\\' && c->s[c->i + 1] == 'u') {
                    c->i += 2;
                    if (!hex4(c, &lo) || lo < 0xDC00 || lo > 0xDFFF) goto bad;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else goto bad;
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) goto bad;
            put_utf8(&b, cp);
            break;
        }
        default: goto bad;
        }
    }
bad:
    sb_free(&b);
    return 0;
}

static jv *parse_value(cur_t *c, int depth);

static jv *parse_container(cur_t *c, int depth, int obj) {
    if (depth > MAX_DEPTH) return NULL;
    char close = obj ? '}' : ']';
    c->i++;
    jv *v = jnew(obj ? J_OBJ : J_ARR);
    size_t cap = 0;
    skip_ws(c);
    if (c->i < c->n && c->s[c->i] == close) { c->i++; return v; }
    for (;;) {
        char *key = NULL; size_t klen;
        skip_ws(c);
        if (obj) {
            if (!parse_string(c, &key, &klen)) goto fail;
            skip_ws(c);
            if (c->i >= c->n || c->s[c->i] != ':') { free(key); goto fail; }
            c->i++;
        }
        jv *item = parse_value(c, depth + 1);
        if (!item) { free(key); goto fail; }
        if (v->n == cap) {
            cap = cap ? cap * 2 : 8;
            v->items = xrealloc(v->items, cap * sizeof *v->items);
            if (obj) v->keys = xrealloc(v->keys, cap * sizeof *v->keys);
        }
        v->items[v->n] = item;
        if (obj) v->keys[v->n] = key;
        v->n++;
        skip_ws(c);
        if (c->i < c->n && c->s[c->i] == ',') { c->i++; continue; }
        if (c->i < c->n && c->s[c->i] == close) { c->i++; return v; }
        goto fail;
    }
fail:
    json_free(v);
    return NULL;
}

static jv *parse_value(cur_t *c, int depth) {
    skip_ws(c);
    if (c->i >= c->n) return NULL;
    char ch = c->s[c->i];
    if (ch == '{') return parse_container(c, depth, 1);
    if (ch == '[') return parse_container(c, depth, 0);
    if (ch == '"') {
        jv *v = jnew(J_STR);
        if (!parse_string(c, &v->str, &v->slen)) { json_free(v); return NULL; }
        return v;
    }
    if (c->n - c->i >= 4 && !strncmp(c->s + c->i, "true", 4)) { c->i += 4; jv *v = jnew(J_BOOL); v->b = 1; return v; }
    if (c->n - c->i >= 5 && !strncmp(c->s + c->i, "false", 5)) { c->i += 5; return jnew(J_BOOL); }
    if (c->n - c->i >= 4 && !strncmp(c->s + c->i, "null", 4)) { c->i += 4; return jnew(J_NULL); }
    if (ch == '-' || (ch >= '0' && ch <= '9')) {
        char tmp[64]; size_t k = 0;
        while (c->i < c->n && k < sizeof tmp - 1 && strchr("+-.eE0123456789", c->s[c->i])) tmp[k++] = c->s[c->i++];
        tmp[k] = 0;
        char *end; double d = strtod(tmp, &end);
        if (end == tmp || *end) return NULL;
        jv *v = jnew(J_NUM); v->num = d; return v;
    }
    return NULL;
}

jv *json_parse(const char *s, size_t len) {
    cur_t c = { s, len, 0, 0 };
    jv *v = parse_value(&c, 0);
    if (!v) return NULL;
    skip_ws(&c);
    if (c.i != c.n) { json_free(v); return NULL; }
    return v;
}

const jv *json_get(const jv *obj, const char *key) {
    if (!obj || obj->t != J_OBJ) return NULL;
    for (size_t i = 0; i < obj->n; i++) if (!strcmp(obj->keys[i], key)) return obj->items[i];
    return NULL;
}
const char *json_gets(const jv *obj, const char *key, size_t *len) {
    const jv *v = json_get(obj, key);
    if (!v || v->t != J_STR) return NULL;
    if (len) *len = v->slen;
    return v->str;
}
double json_getnum(const jv *obj, const char *key, double dflt) {
    const jv *v = json_get(obj, key);
    return v && v->t == J_NUM ? v->num : dflt;
}
int json_getbool(const jv *obj, const char *key, int dflt) {
    const jv *v = json_get(obj, key);
    return v && v->t == J_BOOL ? v->b : dflt;
}

void json_put_string(sb_t *b, const char *s, size_t n) {
    sb_putc(b, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': sb_puts(b, "\\\""); break;
        case '\\': sb_puts(b, "\\\\"); break;
        case '\n': sb_puts(b, "\\n"); break;
        case '\r': sb_puts(b, "\\r"); break;
        case '\t': sb_puts(b, "\\t"); break;
        default:
            if (c < 0x20) sb_printf(b, "\\u%04x", c);
            else sb_putc(b, (char)c);
        }
    }
    sb_putc(b, '"');
}

void json_put_number(sb_t *b, double d) {
    if (!isfinite(d)) sb_puts(b, "null");
    else if (d == floor(d) && fabs(d) < 1e15) sb_printf(b, "%.0f", d);
    else sb_printf(b, "%.17g", d);
}

void json_write(sb_t *b, const jv *v) {
    switch (v->t) {
    case J_NULL: sb_puts(b, "null"); break;
    case J_BOOL: sb_puts(b, v->b ? "true" : "false"); break;
    case J_NUM: json_put_number(b, v->num); break;
    case J_STR: json_put_string(b, v->str, v->slen); break;
    case J_ARR:
        sb_putc(b, '[');
        for (size_t i = 0; i < v->n; i++) { if (i) sb_putc(b, ','); json_write(b, v->items[i]); }
        sb_putc(b, ']');
        break;
    case J_OBJ:
        sb_putc(b, '{');
        for (size_t i = 0; i < v->n; i++) {
            if (i) sb_putc(b, ',');
            json_put_string(b, v->keys[i], strlen(v->keys[i]));
            sb_putc(b, ':');
            json_write(b, v->items[i]);
        }
        sb_putc(b, '}');
        break;
    }
}
