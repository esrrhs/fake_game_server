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
    runtime.pthread_t self = runtime.pthread_self();
    if (self == 0) runtime.pthread_exit((void *)1);
    runtime.pthread_exit((void *)77);
    return (void *)99;
}

int main() {
    runtime.pthread_t t1;
    void *ret = 0;

    int r = runtime.pthread_create(&t1, 0, worker, 0);
    if (r != 0) return 1;

    runtime.pthread_join(t1, &ret);
    if ((long)ret != 77) return 2;

    return 0;
}
