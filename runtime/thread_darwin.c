/* Darwin thread layer — FakeCC dialect. Selected by filename (see
 * c_file_matches_target in src/pkg.c).
 *
 * This file is the whole reason the arm64 backend can import dylib symbols.
 * The first version of it drove bsdthread_create and friends, the syscalls
 * libpthread is written against, and never worked: bsdthread_register(366)
 * and bsdthread_create(360) both return EINVAL no matter how they are
 * called.  Measured against a plain clang program linked with libpthread,
 * the same syscall with the same arguments fails both before *and* after a
 * successful pthread_create, so it is not a question of argument order,
 * stack alignment or pointer authentication -- the kernel only accepts those
 * calls from a thread libpthread itself set up.
 *
 * So the layer calls libpthread the way any other program does.  The four
 * names below are declared extern and never defined here; the backend sees
 * an unresolved call, finds them on its libSystem import list
 * (emit_is_darwin_import), and reaches them through a dyld bind instead of
 * demanding a definition.  Nothing else in the runtime needs that path:
 * printf, malloc and the string functions are all fakecc's own.
 *
 * The image still takes nothing from libSystem but this primitive -- no
 * errno, no stdout, no allocator -- it only borrows the one thing the
 * kernel refuses to hand out directly. */
package runtime;

typedef unsigned long pthread_t;
typedef unsigned long thread_t;

extern int pthread_create(pthread_t *thread, const void *attr,
                          void *(*start_routine)(void *), void *arg);
extern int pthread_join(pthread_t thread, void **retval);
extern pthread_t pthread_self(void);
extern void pthread_exit(void *retval);

int thread_create(thread_t *thread, void *(*start_routine)(void *), void *arg) {
    return pthread_create((pthread_t *)thread, 0, start_routine, arg);
}

int thread_join(thread_t thread, void **retval) {
    return pthread_join((pthread_t)thread, retval);
}

thread_t thread_self(void) {
    return (thread_t)pthread_self();
}

void thread_exit(void *retval) {
    pthread_exit(retval);
}
