// expect: 0
// gcc_flags: -pthread
// unsupported_on: darwin-arm64
// Thread creation has no counterpart here.  runtime/thread.c is built on
// Linux's __clone plus futex; Darwin has futex at no syscall number, and its
// clone() has no shared-address-space mode (macOS dropped CLONE_VM), so the
// only thing a freestanding image could reach is fork -- a process, not a
// thread.  Working threads on Darwin means linking libSystem, which a
// zero-dependency image deliberately does not do.
//
// Everything up to the create call does work: each case's single-threaded TLS
// assertion passes before pthread_create is reached (tls_init_thread returns
// 2, its second check, not 1).
// An aligned(32) TLS object must stay 32-aligned in a child thread too.
package main;
import runtime;

__thread char tc = 1;
__thread int tx __attribute__((aligned(32)));

void *worker(void *arg) {
    (void)arg;
    tx = 1;
    if (((unsigned long)&tx & 31ul) != 0) return (void *)1;
    return (void *)0;
}

int main(void) {
    runtime.pthread_t t;
    void *r = (void *)2;
    if (runtime.pthread_create(&t, 0, worker, 0) != 0) return 3;
    runtime.pthread_join(t, &r);
    if ((long)r != 0) return 4;
    tx = 1;
    if (((unsigned long)&tx & 31ul) != 0) return 5;
    return 0;
}
