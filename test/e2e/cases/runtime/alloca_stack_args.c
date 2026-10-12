// expect: 0
// An alloca() in a function that also makes a call with stack arguments.
//
// AAPCS64 puts stack arguments at [sp, ...) and grows the callee's frame
// downwards from there, so nothing the call touches may sit above sp.  An
// alloca() leaves its block exactly there, and the outgoing area is addressed
// off sp, so the call wrote its arguments into the caller's block.  With nine
// arguments the callee has a stack argument at all; eight or fewer stay in
// registers and never touch the stack, which is why this starts at nine.
//
// Lowering sp by the size of the argument area for the duration of the call
// moves both the arguments and the callee's frame clear of the block.  Offsets
// inside the area do not change, so the stores are untouched and AAPCS64 is
// satisfied as-is.  sp has to come back afterwards, from a frame slot that
// every alloca rewrites -- a loop can allocate more than once and sp keeps
// moving down, so a value sampled at the call would be stale.
package main;
extern void *alloca(unsigned long n);

/* Nine int parameters: the ninth goes on the stack. */
int nine(int a, int b, int c, int d, int e, int f, int g, int h, int i) {
    return a + b + c + d + e + f + g + h + i;
}

/* Locals of its own, so the callee's frame grows far enough to matter. */
long spill9(int a, int b, int c, int d, int e, int f, int g, int h, int i) {
    long t1 = a, t2 = b, t3 = c, t4 = d, t5 = e;
    long t6 = f, t7 = g, t8 = h, t9 = i;
    return t1 + t2 + t3 + t4 + t5 + t6 + t7 + t8 + t9;
}

/* Eight parameters stay in registers: always fine, and it guards against a
 * change that would push the area onto the block regardless. */
int eight(int a, int b, int c, int d, int e, int f, int g, int h) {
    return a + b + c + d + e + f + g + h;
}

int check_one(int n) {
    char *p = (char *)alloca(256);
    for (int i = 0; i < 256; i++) p[i] = (char)i;
    nine(1, 2, 3, 4, 5, 6, 7, 8, n);
    for (int i = 0; i < 256; i++)
        if (p[i] != (char)i) return 1;
    return 0;
}

/* Two calls around one block: the second must not disturb it either. */
int check_twice(int n) {
    char *p = (char *)alloca(256);
    for (int i = 0; i < 256; i++) p[i] = (char)i;
    nine(1, 2, 3, 4, 5, 6, 7, 8, n);
    for (int i = 0; i < 256; i++)
        if (p[i] != (char)i) return 2;
    nine(1, 2, 3, 4, 5, 6, 7, 8, n);
    for (int i = 0; i < 256; i++)
        if (p[i] != (char)i) return 3;
    return 0;
}

/* Two blocks: the one adjacent to the outgoing area is the one at risk. */
int check_two_blocks(int n) {
    char *p = (char *)alloca(128);
    char *q = (char *)alloca(64);
    for (int i = 0; i < 128; i++) p[i] = (char)i;
    for (int i = 0; i < 64; i++) q[i] = (char)i;
    nine(1, 2, 3, 4, 5, 6, 7, 8, n);
    for (int i = 0; i < 128; i++)
        if (p[i] != (char)i) return 4;
    for (int i = 0; i < 64; i++)
        if (q[i] != (char)i) return 5;
    return 0;
}

/* A callee whose own frame has to clear the block. */
int check_spilling_callee(int n) {
    char *p = (char *)alloca(256);
    for (int i = 0; i < 256; i++) p[i] = (char)i;
    spill9(1, 2, 3, 4, 5, 6, 7, 8, n);
    for (int i = 0; i < 256; i++)
        if (p[i] != (char)i) return 6;
    return 0;
}

/* Live values across the call.  The frame slot holding sp must not land on
 * the spill area: prologue writes it before anything else runs, so an overlap
 * would clobber a spilled value and the restore would load garbage. */
int check_spill_pressure(int n) {
    char *p = (char *)alloca(64);
    for (int i = 0; i < 64; i++) p[i] = (char)i;
    long v1 = 1, v2 = 2, v3 = 3, v4 = 4, v5 = 5, v6 = 6, v7 = 7, v8 = 8;
    long r = spill9(1, 2, 3, 4, 5, 6, 7, 8, n);
    long sum = v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + r;
    for (int i = 0; i < 64; i++)
        if (p[i] != (char)i) return 7;
    return sum == 72 + n ? 0 : 8;
}

/* alloca() inside a branch: on the paths that do not allocate, sp stays where
 * the prologue left it, and the slot still has to restore it correctly.
 * Only the named buffer is checked -- the allocated block is fresh stack and
 * its contents are not ours to assert on. */
int check_conditional(int n) {
    char buf[32];
    for (int i = 0; i < 32; i++) buf[i] = (char)i;
    char *r = n == 1 ? (char *)alloca(8) : buf;
    nine(1, 2, 3, 4, 5, 6, 7, 8, n);
    for (int i = 0; i < 32; i++)
        if (buf[i] != (char)i) return 9;
    r[0] = 1;
    return r[0] == 1 ? 0 : 10;
}

/* A call before the first alloca in program order must not restore from an
 * unwritten slot. */
int check_call_before_alloca(int n) {
    char buf[32];
    for (int i = 0; i < 32; i++) buf[i] = (char)i;
    nine(1, 2, 3, 4, 5, 6, 7, 8, n);
    for (int i = 0; i < 32; i++)
        if (buf[i] != (char)i) return 11;
    char *p = (char *)alloca(64);
    for (int i = 0; i < 64; i++) p[i] = (char)i;
    nine(1, 2, 3, 4, 5, 6, 7, 8, n);
    for (int i = 0; i < 64; i++)
        if (p[i] != (char)i) return 12;
    return 0;
}

/* alloca() in a loop moves sp further down each time, so a value taken once
 * would go stale and restore sp into the middle of a block. */
int check_loop(int n) {
    for (int k = 0; k < 4; k++) {
        char *p = (char *)alloca(64);
        for (int i = 0; i < 64; i++) p[i] = (char)i;
        nine(1, 2, 3, 4, 5, 6, 7, 8, n + k);
        for (int i = 0; i < 64; i++)
            if (p[i] != (char)i) return 13;
    }
    return 0;
}

/* A VLA lowers to the same instruction as alloca and moves sp the same way. */
int check_vla(int n) {
    char v[n];
    for (int i = 0; i < n; i++) v[i] = (char)i;
    nine(1, 2, 3, 4, 5, 6, 7, 8, n);
    for (int i = 0; i < n; i++)
        if (v[i] != (char)i) return 14;
    return 0;
}

/* The ninth argument has to reach the callee, not just leave the block alone:
 * checking only the block would pass if the argument were dropped. */
int check_argument_value(void) {
    char *p = (char *)alloca(256);
    for (int i = 0; i < 256; i++) p[i] = (char)i;
    int r = nine(1, 2, 3, 4, 5, 6, 7, 8, 9);
    for (int i = 0; i < 256; i++)
        if (p[i] != (char)i) return 15;
    return r == 45 ? 0 : 16;
}

int main(void) {
    for (int n = 0; n < 4; n++) {
        int r = check_one(n);
        if (r != 0) return r;
        r = check_twice(n);
        if (r != 0) return r;
        r = check_two_blocks(n);
        if (r != 0) return r;
        r = check_spilling_callee(n);
        if (r != 0) return r;
        r = check_spill_pressure(n);
        if (r != 0) return r;
        r = check_conditional(n);
        if (r != 0) return r;
        r = check_call_before_alloca(n);
        if (r != 0) return r;
        r = check_loop(n);
        if (r != 0) return r;
        r = check_vla(8 + n);
        if (r != 0) return r;
        r = eight(1, 2, 3, 4, 5, 6, 7, n);
        if (r != 28 + n) return 17;
    }
    if (check_argument_value() != 0) return 20;
    return 0;
}