/* main.c -- winless entry point.  Copyright (C) 2026 David Walther.  GPL-3.0-or-later. */
#define _POSIX_C_SOURCE 200809L
#include "winless.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *base = strrchr(argv[0], '/'); base = base ? base + 1 : argv[0];
    if (!strcmp(base, "sshw")) return remote_ssh_main(argc - 1, argv + 1);        /* installed as a second name: `sshw HOST` is ssh with winless working on HOST */
    int port = DEFAULT_PORT, explicit_port = 0;
    const char *env = getenv("WINLESS_PORT");
    if (env && *env) { port = atoi(env); explicit_port = 1; }
    int i = 1;
    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) { port = atoi(argv[i + 1]); explicit_port = 1; i += 2; }
        else if (!strcmp(argv[i], "--version")) { puts("winless " PACKAGE_VERSION); return 0; }
        else if (!strcmp(argv[i], "--features")) { puts("bell"); return 0; }    /* what `winless ssh` may rely on when it finds this on a remote host */
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { char *h[] = { "help", NULL }; return client_main(port, 1, 1, h); }
        else { fprintf(stderr, "winless: unknown option %s (try --help)\n", argv[i]); return 2; }
    }
    if (port < 1 || port > 65535) { fprintf(stderr, "winless: bad port\n"); return 2; }
    return client_main(port, explicit_port, argc - i, argv + i);
}
