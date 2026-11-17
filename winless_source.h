/* winless_source.h -- the interface of a winless "source": a library that answers the questions a list or stream window asks
 * (what winless otherwise asks a shell command, see `open --hook`).  A source is loaded with `winless open --source NAME:SPEC`.
 *
 * A source is a shared library NAME.so (found in $WINLESS_SOURCE_DIR, ~/.local/share/winless/sources, or the package's
 * sources directory) that exports wl_source_entry(), or an object linked into winless itself (`./configure --with-source=...`,
 * which the static build that `sshw` uploads needs).
 *
 * Memory: *out and *err are allocated by the source with malloc and freed by winless with free.  The source must not exit or
 * abort the process: report a failure through *err.  fetch is called from winless's one thread, one call at a time.
 *
 * Copyright (C) 2026 David Walther.  GPL-3.0-or-later. */
#ifndef WINLESS_SOURCE_H
#define WINLESS_SOURCE_H

#define WL_SOURCE_ABI 1

typedef struct wl_source {
    int abi;                                       /* WL_SOURCE_ABI */
    const char *name;                              /* the NAME of `--source NAME:SPEC` */
    /* Open the data named by SPEC (whatever follows "NAME:").  NULL, with a message in *err, on failure. */
    void *(*open)(const char *spec, char **err);
    /* The text for ARG (the page's request, e.g. "thread:468" or "stream:speaker:you") and the free text the page sent along
     * (a search or limit pattern; "" if none).  0 on success with *out the UTF-8 text; -1 with a message in *err. */
    int (*fetch)(void *handle, const char *arg, const char *text, char **out, char **err);
    void (*close)(void *handle);
} wl_source;

/* What a source library exports (by this name), and what winless calls to get its description. */
typedef const wl_source *(*wl_source_entry_fn)(void);
#define WL_SOURCE_ENTRY "wl_source_entry"

#endif
