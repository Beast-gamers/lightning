#define _GNU_SOURCE
#include <signal.h>
#include <execinfo.h>
#include <stdio.h>
#include <unistd.h>
static void h(int s){ void*b[32]; int n=backtrace(b,32); fprintf(stderr,"[SEGV] frames=%d\n",n); backtrace_symbols_fd(b,n,2); _exit(139);} 
int main(){struct sigaction sa; sa.sa_handler=h; sigemptyset(&sa.sa_mask); sa.sa_flags=SA_RESETHAND; sigaction(SIGSEGV,&sa,0); *(int*)0=0;}
