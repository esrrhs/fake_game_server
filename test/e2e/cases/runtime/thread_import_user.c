// expect: 0
// gcc_flags: -pthread
// unsupported_on: linux-x86_64 linux-aarch64
// The program's own translation unit declares the thread entry points
// extern and calls them.  Nothing in the image defines them: on Darwin the
// backend binds them from libSystem at load time, which is the only way a
// freestanding image can get a thread here (see runtime/thread_darwin.c).
// Linux resolves the same names at static link time instead, so this case
// belongs to the arm64 port only.
package main;

extern int pthread_create(unsigned long *, const void *,
                          void *(*)(void *), void *);
extern int pthread_join(unsigned long, void **);
extern unsigned long pthread_self(void);
extern void pthread_exit(void *);

static void *add_ten(void *arg) {
    if (pthread_self() == 0) pthread_exit((void *)1);
    return (void *)((long)arg + 10);
}

int main() {
    unsigned long th;
    void *ret = 0;

    if (pthread_create(&th, 0, add_ten, (void *)32) != 0) return 1;
    if (pthread_join(th, &ret) != 0) return 2;
    if ((long)ret != 42) return 3;

    return 0;
}
