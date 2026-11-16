#define _GNU_SOURCE
#include <signal.h>
#include <execinfo.h>
#include <stdio.h>
#include <unistd.h>
int ltng_orig_main(int argc, char **argv);
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
    return ltng_orig_main(argc, argv);
}
