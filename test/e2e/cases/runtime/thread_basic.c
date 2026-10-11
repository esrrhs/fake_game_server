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
package main;

import runtime;

void *add_ten(void *arg) {
    long x = (long)arg;
    return (void *)(x + 10);
}

int main() {
    runtime.thread_t th;
    void *ret = 0;

    int r = runtime.thread_create(&th, add_ten, (void *)32);
    if (r != 0) return 1;

    runtime.thread_join(th, &ret);
    if ((long)ret != 42) return 2;

    return 0;
}
