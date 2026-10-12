// expect: 0
// Pointer-authentication instructions.
//
// Darwin signs a thread start routine before handing it to the kernel and
// authenticates on return, so a runtime that talks to bsdthread_* directly
// needs the same pair.  These are HINT-space instructions: on a process that
// did not opt in at exec time they behave as no-ops, which is what makes them
// safe to emit unconditionally and what the checks below rely on.
//
// The encodings sit in the same space as ret, so a wrong word would decode as
// a neighbouring instruction rather than fail the build.  These cases pin the
// behaviour a wrong encoding would break: the wrong register clobbered, a
// trap, or a value that does not come back.

package main;

extern void __paciasp(void);
extern void __retab(void);
extern long __pacia(long);
extern long __autda(long, long);

extern int printf(const char *, ...);

/* An address with a stable value, distinct from every local. */
int global_anchor;

/* The platform's own pair -- authenticate the return address on entry, come
 * back with retab -- brackets the *whole function*.  It cannot be exercised by
 * calling the two intrinsics from inside one: retab then has no matching
 * paciasp to verify the return address against, and on a process that opted in
 * the failure is not a clean trap but an undefined one.  Emitting the pair is
 * the backend's job at the prologue and epilogue, so this case pins the two
 * instructions individually and leaves the pairing to that. */

/* Sign a pointer and read it back.  Where the instructions are no-ops the
 * address comes back unchanged; where they are not, the signature has to
 * survive the pair.  Either way the address is what returns. */
static long roundtrip(long p) {
    long signed_ptr = __pacia(p);
    return __autda(signed_ptr, 0);
}

static int check(long p, const char *what) {
    long r = roundtrip(p);
    if (r != p) {
        printf("%s: %lx != %lx\n", what, r, p);
        return 1;
    }
    return 0;
}

int main(void) {
    int bad = 0;
    long local = 0;
    long stack_array[4];

    /* paciasp on its own is harmless.  Its partner, retab, is not exercised
     * here for the reason given above. */
    __paciasp();

    /* Distinct address kinds, so an instruction writing the wrong register
     * shows up instead of passing by luck. */
    bad |= check((long)&global_anchor, "global");
    bad |= check((long)&local, "local");
    bad |= check((long)stack_array, "stack array");
    bad |= check((long)&main, "function");
    bad |= check((long)&printf, "external function");

    /* autda must return its first argument: with two distinct arguments an
     * implementation that returned the second is caught. */
    long second = 0x2222;
    long first = (long)&second;
    long signed_first = __pacia(first);
    long got = __autda(signed_first, second);
    if (got != first) {
        printf("autda returned the wrong operand: %lx != %lx\n", got, first);
        bad = 1;
    }

    return bad;
}
