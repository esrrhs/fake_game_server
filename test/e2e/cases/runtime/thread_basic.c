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
