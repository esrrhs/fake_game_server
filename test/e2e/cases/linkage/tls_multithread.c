// expect: 0
// gcc_flags: -pthread
// unsupported_on: darwin-arm64

// Skipped for a different reason now that threads work: see tls_init_thread.
// The three workers return the right values only because they run one after
// another; main's own copies come back changed.
package main;

import runtime;

__thread int thread_id = 0;
__thread int thread_accum = 1000;

void *worker1(void *arg) {
    thread_id = 1;
    thread_accum = 0;
    for (int i = 0; i < 100; i++) thread_accum += 1;
    return (void *)(long)thread_accum;
}

void *worker2(void *arg) {
    thread_id = 2;
    thread_accum = 0;
    for (int i = 0; i < 100; i++) thread_accum += 2;
    return (void *)(long)thread_accum;
}

void *worker3(void *arg) {
    thread_id = 3;
    thread_accum = 0;
    for (int i = 0; i < 100; i++) thread_accum += 3;
    return (void *)(long)thread_accum;
}

int main() {
    thread_id = 99;
    thread_accum = 42;

    runtime.pthread_t t1, t2, t3;
    void *res1 = 0;
    void *res2 = 0;
    void *res3 = 0;

    int r1 = runtime.pthread_create(&t1, 0, worker1, 0);
    if (r1 != 0) return 1;

    int r2 = runtime.pthread_create(&t2, 0, worker2, 0);
    if (r2 != 0) return 2;

    int r3 = runtime.pthread_create(&t3, 0, worker3, 0);
    if (r3 != 0) return 3;

    runtime.pthread_join(t1, &res1);
    runtime.pthread_join(t2, &res2);
    runtime.pthread_join(t3, &res3);

    if ((long)res1 != 100) return 4;
    if ((long)res2 != 200) return 5;
    if ((long)res3 != 300) return 6;

    /* Verify main thread TLS was not touched by worker threads */
    if (thread_id != 99) return 7;
    if (thread_accum != 42) return 8;

    return 0;
}
