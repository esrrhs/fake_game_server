// expect: 0
// gcc_flags: -pthread
// unsupported_on: darwin-arm64

// Skipped for a different reason now that threads work: Darwin has no
// per-thread copy of a __thread object yet.  The image carries one template
// and the descriptor's thunk returns its address, so every thread sees the
// same storage -- here thread 2 reads 133 where it expects 123.  Real
// isolation needs the descriptor to live in __DATA,__thread_vars with its
// thunk bound to _tlv_bootstrap, which dyld then swaps for a per-thread
// address.
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
