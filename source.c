/* source.c -- finding a source (winless_source.h): built into this binary, or a NAME.so on disk.
 * Copyright (C) 2026 David Walther.  GPL-3.0-or-later. */
#define _POSIX_C_SOURCE 200809L
#include "winless.h"
#include "winless_source.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef WL_STATIC
#include <dlfcn.h>
#endif

#ifdef WL_BUILTIN_SOURCE
extern const wl_source *wl_source_entry(void);          /* linked in: ./configure --with-source=NAME:LIBDIR */
#endif

#ifndef PKGLIBDIR
#define PKGLIBDIR "/usr/local/lib/winless"
#endif

/* where NAME.so is looked for, in order */
static size_t search_dirs(char dirs[3][700]) {
    size_t n = 0;
    const char *e = getenv("WINLESS_SOURCE_DIR");
    if (e && *e) snprintf(dirs[n++], 700, "%s", e);
    char d[600]; winless_data_dir(d, sizeof d);
    snprintf(dirs[n++], 700, "%s/sources", d);
    snprintf(dirs[n++], 700, "%s/sources", PKGLIBDIR);
    return n;
}

static int name_ok(const char *name) {
    if (!name || !*name || strlen(name) > 40) return 0;
    for (const char *p = name; *p; p++) if (!(islower((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '_' || *p == '-')) return 0;
    return 1;
}

static char *errdup(const char *fmt, const char *a) { sb_t b; sb_init(&b); sb_printf(&b, fmt, a); return sb_detach(&b, NULL); }

const wl_source *source_find(const char *name, char **err) {
    *err = NULL;
    if (!name_ok(name)) { *err = errdup("bad source name \"%s\" (lower-case letters, digits, _ and - only)", name ? name : ""); return NULL; }
#ifdef WL_BUILTIN_SOURCE
    { const wl_source *s = wl_source_entry(); if (s && s->abi == WL_SOURCE_ABI && !strcmp(s->name, name)) return s; }
#endif
#ifdef WL_STATIC
    *err = errdup("no source \"%s\" in this build (a static build cannot load libraries)", name);
    return NULL;
#else
    char dirs[3][700]; size_t nd = search_dirs(dirs);
    for (size_t i = 0; i < nd; i++) {
        sb_t pb; sb_init(&pb); sb_printf(&pb, "%s/%s.so", dirs[i], name);
        char *path = sb_detach(&pb, NULL);
        if (access(path, R_OK) != 0) { free(path); continue; }
        void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!h) { free(*err); *err = errdup("cannot load %s", path); free(path); continue; }
        wl_source_entry_fn fn;
        *(void **)(&fn) = dlsym(h, WL_SOURCE_ENTRY);        /* the POSIX way to turn dlsym's pointer into a function pointer */
        const wl_source *s = fn ? fn() : NULL;
        if (!s || s->abi != WL_SOURCE_ABI || !s->name || strcmp(s->name, name)) {
            free(*err); *err = errdup("%s is not a winless source of this version", path); free(path); dlclose(h); continue;
        }
        free(path); free(*err); *err = NULL;
        return s;                                          /* the library stays loaded for the life of the server */
    }
    if (!*err) *err = errdup("no source \"%s\" found (looked for %s.so in $WINLESS_SOURCE_DIR, the user's winless/sources and the package's)", name);
    return NULL;
#endif
}

/* The names of the sources this winless can use, for --features ("source:NAME"). */
void source_features(void) {
    const char *builtin = NULL;
#ifdef WL_BUILTIN_SOURCE
    { const wl_source *s = wl_source_entry(); if (s && s->abi == WL_SOURCE_ABI) { builtin = s->name; printf("source:%s\n", s->name); } }
#endif
#ifndef WL_STATIC
    puts("source");
    char dirs[3][700]; size_t nd = search_dirs(dirs);
    for (size_t i = 0; i < nd; i++) {
        DIR *d = opendir(dirs[i]); if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            size_t l = strlen(e->d_name);
            if (l < 4 || strcmp(e->d_name + l - 3, ".so")) continue;
            char nm[64]; if (l - 3 >= sizeof nm) continue;
            memcpy(nm, e->d_name, l - 3); nm[l - 3] = 0;
            if (name_ok(nm) && !(builtin && !strcmp(nm, builtin))) printf("source:%s\n", nm);
        }
        closedir(d);
    }
#endif
}
