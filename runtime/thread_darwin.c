/* Darwin thread layer — FakeCC dialect. Selected by filename (see
 * c_file_matches_target in src/pkg.c).
 *
 * thread.c is built on Linux primitives: __clone() with a hand-built stack and
 * a TLS image the thread starts on.  None of that exists here, so this file
 * reaches the Darwin equivalents instead -- bsdthread_create and friends,
 * which is the same interface libpthread itself is written against.
 *
 * Everything below is expressed with __syscall rather than by calling into
 * libSystem: the arm64 backend inlines the BSD syscalls directly (see
 * emit_bound_builtin in src/cg64.c), so a runtime that calls the C library
 * for them would depend on symbols the backend resolves as instructions.
 *
 * The Darwin numbers, for reference:
 *   bsdthread_create     360
 *   bsdthread_terminate  361
 *   bsdthread_register   366
 *   bsdthread_ctl        478
 */
package runtime;

extern long __syscall(long, ...);

/* Pointer authentication.  The thread start routine has to be signed before
 * the kernel gets it and authenticated on the way back; a process that did not
 * opt in executes both as no-ops. */
extern long __pacia(long);
extern void __paciasp(void);

/* MAP_PRIVATE|MAP_ANONYMOUS on Darwin, and PROT_READ|PROT_WRITE.  Not the
 * Linux values: Linux gives MAP_ANONYMOUS 0x20 and this platform 0x1000, so
 * passing 0x22 asks for MAP_PRIVATE plus an undefined bit and no anonymous
 * mapping at all.  Spelled out rather than #defined because this dialect has
 * no preprocessor (see README: #define is deliberately gone). */
static const long DARWIN_MAP_PRIVATE = 0x0002;
static const long DARWIN_MAP_ANON = 0x1000;
static const long DARWIN_PROT_RW = 0x3;

/* bsdthread_create's PTHREAD_START_CUSTOM: the new thread resumes at the
 * address given here rather than at a kernel-chosen entry. */
static const long DARWIN_THREAD_CUSTOM = 0x01000000;

/* Types are per-file in this dialect -- a package does not share struct
 * definitions between its files -- so each platform file carries its own,
 * matching how the rest of the runtime is written.  Only the TLS image is
 * shared, because the linker has to find it by name across translation units. */
typedef unsigned long pthread_t;
typedef unsigned long thread_t;

struct __fakecc_thread {
    void *(*start_routine)(void *);
    void *arg;
    void *retval;
    void *stack_base;
    unsigned long stack_size;
    unsigned long tid;
    int done;
};

/* The main thread's record.  A handle is this address, so pthread_self() has
 * something stable to return. */
struct __fakecc_thread __fakecc_thread_self;

/* The kernel resumes a new thread at this with the arguments bsdthread_create
 * was given still in x0..x5 -- no frame is pushed, so nothing can be spilled
 * and there is no return address to come back to.  It runs the user routine,
 * records the result, and leaves through bsdthread_terminate, which does not
 * return. */
static long __fakecc_thread_entry(long arg, long a2, long a3,
                                  long a4, long a5, long a6) {
    struct __fakecc_thread *t = (struct __fakecc_thread *)arg;
    t->retval = t->start_routine(t->arg);
    t->done = 1;
    __syscall(361 /* bsdthread_terminate */, 0, 0, 0, 0, 0, 0);
    return 0;
}

/* Reserve a page-aligned stack.  bsdthread_create takes the top of it and
 * builds down, so the mapping has to be at least 16-byte aligned for the
 * frame the kernel sets up. */
static void *__fakecc_alloc_stack(unsigned long size) {
    unsigned long raw = (unsigned long)__syscall(
        197 /* mmap */, 0, (long)size, DARWIN_PROT_RW,
        DARWIN_MAP_PRIVATE | DARWIN_MAP_ANON, -1, 0);
    if ((long)raw < 0) return 0;
    /* Trim the slack so the mapping is exactly the size asked for; a partial
     * unmapping is harmless when the whole thing is being released anyway. */
    unsigned long end = (raw + size + 4095) & ~4095UL;
    if (end > raw + size)
        __syscall(73 /* munmap */, (long)(raw + size), (long)(end - raw - size));
    return (void *)raw;
}

static void __fakecc_free_stack(void *base, unsigned long size) {
    if (base && size)
        __syscall(73 /* munmap */, (long)base, (long)size);
}

/* Register the entry point with the kernel once.  Without it a new thread has
 * nothing to resume into, and the call is rejected. */
static int __fakecc_register_thread_entry(void) {
    static int registered = 0;
    if (registered) return 0;
    long r = __syscall(366 /* bsdthread_register */,
                       __pacia((long)__fakecc_thread_entry),
                       0,      /* wqthread */
                       0x3c0,  /* pthsize */
                       0,      /* pthread structure: allocated per thread */
                       0x3c,   /* dummy */
                       0xa0);   /* dispatch queue offset */
    if (r < 0) return (int)r;
    registered = 1;
    return 0;
}

int pthread_create(pthread_t *thread, const void *attr,
                   void *(*start_routine)(void *), void *arg) {
    if (!thread || !start_routine) return 22 /* EINVAL */;

    int rc = __fakecc_register_thread_entry();
    if (rc != 0) return rc;

    unsigned long size = 512 * 1024;
    void *stack = __fakecc_alloc_stack(size);
    if (!stack) return 12 /* ENOMEM */;

    struct __fakecc_thread *t = (struct __fakecc_thread *)__fakecc_alloc_stack(
        4096);
    if (!t) {
        __fakecc_free_stack(stack, size);
        return 12 /* ENOMEM */;
    }
    t->start_routine = start_routine;
    t->arg = arg;
    t->retval = 0;
    t->stack_base = stack;
    t->stack_size = size;
    t->tid = 0;
    t->done = 0;

    long r = __syscall(360 /* bsdthread_create */,
                       __pacia((long)__fakecc_thread_entry),
                       (long)t,
                       (long)((char *)stack + size),
                       DARWIN_THREAD_CUSTOM, 0, 0);
    if (r < 0) {
        __fakecc_free_stack(t, 4096);
        __fakecc_free_stack(stack, size);
        return 22 /* EINVAL */;
    }
    t->tid = (unsigned long)r;
    *thread = (pthread_t)t;
    return 0;
}

int thread_create(thread_t *thread, void *(*start_routine)(void *), void *arg) {
    return pthread_create((pthread_t *)thread, 0, start_routine, arg);
}

int pthread_join(pthread_t thread, void **retval) {
    struct __fakecc_thread *t = (struct __fakecc_thread *)thread;
    if (!t) return 22 /* EINVAL */;

    /* bsdthread_ctl(1, BSDTHREAD_CTL_JOIN) parks the caller on the thread's
     * exit; when it returns the routine has finished. */
    __syscall(478 /* bsdthread_ctl */, 1 /* join */, t->tid, 0, 0, 0, 0);

    if (retval) *retval = t->retval;

    void *sb = t->stack_base;
    unsigned long ss = t->stack_size;
    t->stack_base = 0;
    __fakecc_free_stack(t, 4096);
    __fakecc_free_stack(sb, ss);
    return 0;
}

int thread_join(thread_t thread, void **retval) {
    return pthread_join((pthread_t)thread, retval);
}

pthread_t pthread_self(void) {
    return (pthread_t)&__fakecc_thread_self;
}

thread_t thread_self(void) {
    return (thread_t)pthread_self();
}

void pthread_exit(void *retval) {
    struct __fakecc_thread *t = (struct __fakecc_thread *)pthread_self();
    if (t && t != &__fakecc_thread_self) {
        t->retval = retval;
        t->done = 1;
    }
    __syscall(361 /* bsdthread_terminate */, 0, 0, 0, 0, 0, 0);
}

void thread_exit(void *retval) {
    pthread_exit(retval);
}
