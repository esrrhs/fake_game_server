// expect: 0
// gcc_flags: -pthread

// An aligned(32) TLS object must stay 32-aligned in a child thread too.
//
// Threads exist on Darwin now, so this runs.  It checks alignment only:
// per-thread isolation is tls_init_thread's and tls_multithread's job, and
// they are still skipped (see there).
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
