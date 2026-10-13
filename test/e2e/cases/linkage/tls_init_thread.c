// expect: 0
// gcc_flags: -pthread

// Darwin has no per-thread __thread object of its own to fall back on, so
// this goes through the native TLV path: a tlv_descriptor in
// __DATA,__thread_vars with its thunk bound to _tlv_bootstrap, which dyld
// rewrites into a function returning this thread's block.  The storage is
// the copy of __thread_data each thread gets at bootstrap, so main's copy
// survives the workers untouched.
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
