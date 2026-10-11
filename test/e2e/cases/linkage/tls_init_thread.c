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

__thread int initial_val = 123;

void *worker(void *arg) {
    long id = (long)arg;
    if (initial_val != 123) return (void *)1;
    initial_val += id;
    return (void *)(long)initial_val;
}

int main() {
    if (initial_val != 123) return 1;

    runtime.pthread_t t1, t2;
    void *r1 = 0;
    void *r2 = 0;

    int err1 = runtime.pthread_create(&t1, 0, worker, (void *)10);
    if (err1 != 0) return 2;

    int err2 = runtime.pthread_create(&t2, 0, worker, (void *)20);
    if (err2 != 0) return 3;

    runtime.pthread_join(t1, &r1);
    runtime.pthread_join(t2, &r2);

    if ((long)r1 != 133) return 4;
    if ((long)r2 != 143) return 5;
    if (initial_val != 123) return 6;

    return 0;
}
