#define _GNU_SOURCE
#include <signal.h>
#include <execinfo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void segv_handler(int sig) {
    void *bt[32];
    int n = backtrace(bt, 32);
    fprintf(stderr, "[SEGV] backtrace (%d frames):\n", n);
    backtrace_symbols_fd(bt, n, 2);
    _exit(139);
}

int main(int argc, char **argv) {
    struct sigaction sa;
    sa.sa_handler = segv_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;
    sigaction(SIGSEGV, &sa, &sa);
    if (argc < 2) return 2;
    char **newargv = (char **)malloc(sizeof(char *) * (argc + 1));
    int i;
    newargv[0] = argv[1];
    for (i = 1; i < argc; i++) newargv[i] = argv[i + 1];
    newargv[argc] = NULL;
    execv(argv[1], newargv);
    fprintf(stderr, "[SEGV] execv failed: %s\n", argv[1]);
    return 127;
}
