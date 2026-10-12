// expect: 0
// gcc_flags: -pthread
// Threads on Darwin come from libpthread: the runtime declares
// pthread_create/join/self/exit extern, and the arm64 backend binds them
// from libSystem at load time.  Building them from syscalls instead is not
// possible -- bsdthread_register(366) and bsdthread_create(360) both return
// EINVAL from any thread libpthread did not create, including one that has
// already called pthread_create successfully.
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
