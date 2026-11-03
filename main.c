/* main.c -- winless entry point.  Copyright (C) 2026 David Walther.  GPL-3.0-or-later. */
#define _POSIX_C_SOURCE 200809L
#include "winless.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    int port = DEFAULT_PORT;
    const char *env = getenv("WINLESS_PORT");
    if (env && *env) port = atoi(env);
    int i = 1;
    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) { port = atoi(argv[i + 1]); i += 2; }
        else if (!strcmp(argv[i], "--version")) { puts("winless " PACKAGE_VERSION); return 0; }
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { char *h[] = { "help", NULL }; return client_main(port, 1, h); }
        else { fprintf(stderr, "winless: unknown option %s (try --help)\n", argv[i]); return 2; }
    }
    if (port < 1 || port > 65535) { fprintf(stderr, "winless: bad port\n"); return 2; }
    return client_main(port, argc - i, argv + i);
}
