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

void *worker(void *arg) {
    long id = (long)arg;
    runtime.pthread_t self = runtime.pthread_self();
    if (self == 0) runtime.pthread_exit((void *)1);
    return (void *)(id * 2);
}

int main() {
    runtime.pthread_t threads[150];
    for (int i = 0; i < 150; i++) {
        int r = runtime.pthread_create(&threads[i], 0, worker, (void *)(long)i);
        if (r != 0) return 1;
    }
    for (int i = 0; i < 150; i++) {
        void *ret = 0;
        int r = runtime.pthread_join(threads[i], &ret);
        if (r != 0) return 2;
        if ((long)ret != (long)(i * 2)) return 3;
    }
    return 0;
}
